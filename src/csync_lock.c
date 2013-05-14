/*
 * libcsync -- a library to sync a directory with another
 *
 * Copyright (c) 2006      by Andreas Schneider <mail@cynapses.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "config.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <sys/types.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "c_lib.h"
#include "csync_lock.h"
#include "csync.h"

#define CSYNC_LOG_CATEGORY_NAME "csync.lock"
#include "csync_log.h"

#ifndef _WIN32
static int _csync_lock_create(CSYNC *ctx, const char *lockfile) {
  int fd = -1;
  pid_t pid = 0;
  int rc = -1;
  char errbuf[256] = {0};
  char *ctmpfile = NULL;
  char *dir = NULL;
  char *buf = NULL;
  mode_t mask;

  pid = getpid();

  dir = c_dirname(lockfile);
  if (dir == NULL) {
    rc = -1;
    goto out;
  }

  if (asprintf(&ctmpfile, "%s/tmp_lock_XXXXXX", dir) < 0) {
    rc = -1;
    goto out;
  }

  CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE, "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX");
  CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE, "Create temporary lock file: %s", ctmpfile);
  mask = umask(0077);
  fd = mkstemp(ctmpfile);
  umask(mask);
  if (fd < 0) {
    strerror_r(errno, errbuf, sizeof(errbuf));
    CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR,
        "Unable to create temporary lock file: %s - %s",
        ctmpfile,
        errbuf);
    rc = -1;
    goto out;
  }

  CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE, "Write pid (%d) to temporary lock file: %s", pid, ctmpfile);
  pid = asprintf(&buf, "%d\n", pid);
  if (write(fd, buf, pid) == pid) {
    /* Create lock file */
    CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE, "Create a hardlink from %s to %s.", ctmpfile, lockfile);
    if (link(ctmpfile, lockfile) < 0 ) {
      /* Oops, alredy locked */
      strerror_r(errno, errbuf, sizeof(errbuf));
      CSYNC_LOG(CSYNC_LOG_PRIORITY_INFO,
          "Already locked: %s - %s",
          lockfile,
          errbuf);
      rc = -1;
      goto out;
    }
  } else {
    strerror_r(errno, errbuf, sizeof(errbuf));
    CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR,
        "Can't create %s - %s",
        ctmpfile,
        errbuf);
    rc = -1;
    goto out;
  }

  rc = 0;

out:
  if (fd > 0) {
      close(fd);
  }
  if (ctmpfile) {
      unlink(ctmpfile);
  }

  SAFE_FREE(buf);
  SAFE_FREE(dir);
  SAFE_FREE(ctmpfile);

  return rc;
}

static pid_t _csync_lock_read(CSYNC *ctx, const char *lockfile) {
  char errbuf[256] = {0};
  char buf[8] = {0};
  long int tmp;
  ssize_t rc;
  int  fd;
  pid_t pid;
  const _TCHAR *wlockfile;

  /* Read PID from existing lock */
  wlockfile = c_multibyte(lockfile);
  fd = _topen(wlockfile, O_RDONLY);
  c_free_multibyte(wlockfile);

  if( !fd ) {
      return -1;
  }

  rc = read(fd, buf, sizeof(buf));
  close(fd);

  if (rc <= 0) {
     return -1;
  }

  buf[sizeof(buf) - 1] = '\0';
  tmp = strtol(buf, NULL, 10);
  if (tmp == 0 || tmp > 0xFFFF || errno == ERANGE) {
     /* Broken lock file */
     strerror_r(ERANGE, errbuf, sizeof(errbuf));
     if (unlink(lockfile) < 0) {
       CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR,
           "Unable to remove broken lock %s - %s",
           lockfile,
           errbuf);
     }
     return -1;
  }
  pid = (pid_t)(tmp & 0xFFFF);

  /* Check if process is still alive */
  if (kill(pid, 0) < 0 && errno == ESRCH) {
     /* Process is dead. Remove stale lock. */
     if (unlink(lockfile) < 0) {
       strerror_r(errno, errbuf, sizeof(errbuf));
       CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR,
           "Unable to remove stale lock %s - %s",
           lockfile,
           errbuf);
     }
     return -1;
  }

  return pid;
}
#endif

int csync_lock(CSYNC *ctx, const char *lockfile) {
#ifdef _DO_CREATE_A_LOCK_FILE /* disable lock file for ownCloud client, not only _WIN32 */
  /* Check if lock already exists. */
  if (_csync_lock_read(ctx, lockfile) > 0) {
    CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "Aborting, another synchronization process is running.");
    return -1;
  }

  CSYNC_LOG(CSYNC_LOG_PRIORITY_INFO, "Creating lock file: %s", lockfile);

  return _csync_lock_create(ctx, lockfile);
#else
  (void) ctx;
  (void) lockfile;
  return 0;
#endif

}

void csync_lock_remove(CSYNC *ctx, const char *lockfile) {
#ifdef _DO_CREATE_A_LOCK_FILE
#ifndef _WIN32
  char errbuf[256] = {0};
  /* You can't remove the lock if it is from another process */
  if (_csync_lock_read(ctx, lockfile) == getpid()) {
    CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG, "Removing lock file: %s", lockfile);
    if (unlink(lockfile) < 0) {
      strerror_r(errno, errbuf, sizeof(errbuf));
      CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR,
          "Unable to remove lock %s - %s",
          lockfile,
          errbuf);
    }
  }
#endif
#else
  (void) ctx;
  (void) lockfile;
#endif

}

