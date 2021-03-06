/*
 * cynapses libc functions
 *
 * Copyright (c) 2008 by Andreas Schneider <mail@cynapses.org>
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

#ifdef _WIN32
#include <windef.h>
#include <winbase.h>
#endif

#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>

#include "c_file.h"
#include "c_string.h"

#include "c_private.h"

/* check if path is a file */
int c_isfile(const char *path) {
  csync_stat_t sb;
  mbchar_t *wpath = c_utf8_to_locale(path);

  int re = _tstat(wpath, &sb);
  c_free_locale_string(wpath);

  if (re< 0) {
    return 0;
  }

#ifdef __unix__
  if (S_ISREG(sb.st_mode) || S_ISLNK(sb.st_mode)) {
#else
  if (S_ISREG(sb.st_mode)) {
#endif
    return 1;
  }

  return 0;
}

/* copy file from src to dst, overwrites dst */
int c_copy(const char* src, const char *dst, mode_t mode) {
  int srcfd = -1;
  int dstfd = -1;
  int rc = -1;
  ssize_t bread, bwritten;
  csync_stat_t sb;
  char buf[4096];

#ifdef _WIN32
  if(src && dst) {
      mbchar_t *wsrc = c_utf8_to_locale(src);
      mbchar_t *wdst = c_utf8_to_locale(dst);
      if (CopyFileW(wsrc, wdst, FALSE)) {
          rc = 0;
      }
      errno = GetLastError();

      return -1;
  }
#else

  /* Win32 does not come here. */
  if (c_streq(src, dst)) {
    return -1;
  }

  if (lstat(src, &sb) < 0) {
    return -1;
  }

  if (S_ISDIR(sb.st_mode)) {
    errno = EISDIR;
    return -1;
  }

  if (mode == 0) {
    mode = sb.st_mode;
  }

  if (lstat(dst, &sb) == 0) {
    if (S_ISDIR(sb.st_mode)) {
      errno = EISDIR;
      return -1;
    }
  }

  if ((srcfd = open(src, O_RDONLY, 0)) < 0) {
    rc = -1;
    goto out;
  }

  if ((dstfd = open(dst, O_CREAT|O_WRONLY|O_TRUNC, mode)) < 0) {
    rc = -1;
    goto out;
  }

  for (;;) {
    bread = read(srcfd, buf, sizeof(buf));
    if (bread == 0) {
      /* done */
      break;
    } else if (bread < 0) {
      errno = ENODATA;
      rc = -1;
      goto out;
    }

    bwritten = write(dstfd, buf, bread);
    if (bwritten < 0) {
      errno = ENODATA;
      rc = -1;
      goto out;
    }

    if (bread != bwritten) {
      errno = EFAULT;
      rc = -1;
      goto out;
    }
  }

#ifdef __unix__
  fsync(dstfd);
#endif

  rc = 0;
out:
  if (srcfd > 0) {
    close(srcfd);
  }
  if (dstfd > 0) {
      close(dstfd);
  }
  if (rc < 0) {
    unlink(dst);
  }
  return rc;
#endif
}

int c_rename( const char *src, const char *dst ) {
    mbchar_t *nuri;
    mbchar_t *ouri;
    int rc;

    nuri = c_utf8_to_locale(dst);
    if (nuri == NULL) {
        return -1;
    }

    ouri = c_utf8_to_locale(src);
    if (ouri == NULL) {
        return -1;
    }

#ifdef _WIN32
    {
        BOOL ok;
        ok = MoveFileExW(ouri,
                         nuri,
                         MOVEFILE_COPY_ALLOWED +
                         MOVEFILE_REPLACE_EXISTING +
                         MOVEFILE_WRITE_THROUGH);
        if (!ok) {
            /* error */
            rc = -1;
        }
    }
#else
    rc = rename(ouri, nuri);
#endif

    c_free_locale_string(nuri);
    c_free_locale_string(ouri);

    return rc;
}

int c_compare_file( const char *f1, const char *f2 ) {
  mbchar_t *wf1, *wf2;
  int fd1 = -1, fd2 = -1;
  size_t size1, size2;
  char buffer1[BUFFER_SIZE];
  char buffer2[BUFFER_SIZE];
  csync_stat_t stat1;
  csync_stat_t stat2;

  int rc = -1;

  if(f1 == NULL || f2 == NULL) return -1;

  wf1 = c_utf8_to_locale(f1);
  if(wf1 == NULL) {
    return -1;
  }

  wf2 = c_utf8_to_locale(f2);
  if(wf2 == NULL) {
    return -1;
  }

  /* compare size first. */
  rc = _tstat(wf1, &stat1);
  if(rc< 0) {
    goto out;
  }
  rc = _tstat(wf2, &stat2);
  if(rc < 0) {
    goto out;
  }

  /* if sizes are different, the files can not be equal. */
  if( stat1.st_size != stat2.st_size ) {
    rc = 0;
    goto out;
  }

#ifdef _WIN32
  _fmode = _O_BINARY;
#endif

  fd1 = _topen(wf1, O_RDONLY);
  if(fd1 < 0) {
    rc = -1;
    goto out;
  }
  fd2 = _topen(wf2, O_RDONLY);
  if(fd2 < 0) {
    rc = -1;
    goto out;
  }

  while( (size1 = read(fd1, buffer1, BUFFER_SIZE)) > 0 ) {
    size2 = read( fd2, buffer2, BUFFER_SIZE );

    if( size1 != size2 ) {
      rc = 0;
      goto out;
    }
    if(memcmp(buffer1, buffer2, size1) != 0) {
      /* buffers are different */
      rc = 0;
      goto out;
    }
  }

  rc = 1;

out:

  if(fd1 > -1) close(fd1);
  if(fd2 > -1) close(fd2);

  c_free_locale_string( wf1 );
  c_free_locale_string( wf2 );
  return rc;

}
