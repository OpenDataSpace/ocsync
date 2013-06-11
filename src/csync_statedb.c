/*
 * libcsync -- a library to sync a directory with another
 *
 * Copyright (c) 2008      by Andreas Schneider <mail@cynapses.org>
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

#include <sqlite3.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "c_lib.h"
#include "csync_private.h"
#include "csync_statedb.h"
#include "csync_util.h"
#include "csync_misc.h"

#include "c_string.h"

#define CSYNC_LOG_CATEGORY_NAME "csync.statedb"
#include "csync_log.h"

#define BUF_SIZE 16

void csync_set_statedb_exists(CSYNC *ctx, int val) {
  ctx->statedb.exists = val;
}

int csync_get_statedb_exists(CSYNC *ctx) {
  return ctx->statedb.exists;
}

/* Set the hide attribute in win32. That makes it invisible in normal explorers */
static void _csync_win32_hide_file( const char *file ) {
#ifdef _WIN32
  _TCHAR *fileName;
  if( !file ) return;

  fileName = c_multibyte( file );

  DWORD dwAttrs = GetFileAttributesW(fileName);

  if (dwAttrs==INVALID_FILE_ATTRIBUTES) return;

  if (!(dwAttrs & FILE_ATTRIBUTE_HIDDEN)) {
     SetFileAttributesW(fileName, dwAttrs | FILE_ATTRIBUTE_HIDDEN );
  }

  c_free_multibyte(fileName);
#else
    (void) file;
#endif
}

static int _csync_check_db_integrity(CSYNC *ctx) {
    c_strlist_t *result = NULL;
    int rc = -1;

    if (ctx == NULL) {
        return -1;
    }
    if (ctx->statedb.db == NULL) {
        return -1;
    }

    result = csync_statedb_query(ctx, "PRAGMA quick_check;");
    if (result != NULL) {
        /* There is  a result */
        if (result->count > 0) {
            if (c_streq(result->vector[0], "ok")) {
                rc = 0;
            }
        }
        c_strlist_destroy(result);
    }

    return rc;

}


static int _csync_statedb_check(CSYNC *ctx, const char *statedb) {
    int fd = -1, rc;
    ssize_t r;
    char buf[BUF_SIZE] = {0};
    const _TCHAR *wstatedb;

    /* check db version */
#ifdef _WIN32
    _fmode = _O_BINARY;
#endif

    wstatedb = c_multibyte(statedb);
    fd = _topen(wstatedb, O_RDONLY);

    if (fd >= 0) {
        r = read(fd, (void *) buf, sizeof(buf) - 1);
        close(fd);
        if (r >= 0) {
            buf[BUF_SIZE - 1] = '\0';
            if (c_streq(buf, "SQLite format 3")) {
                if (sqlite3_open(statedb, &ctx->statedb.db ) == SQLITE_OK) {
                    rc = _csync_check_db_integrity(ctx);
                    sqlite3_close(ctx->statedb.db);
                    ctx->statedb.db = 0;

                    if( rc >= 0 ) {
                        /* everything is fine */
                        c_free_multibyte(wstatedb);
                        return 0;
                    }
                    CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "Integrity check failed!");
                } else {
                    // FIXME: Better error analysis.
                    CSYNC_LOG(CSYNC_LOG_PRIORITY_WARN, "database corrupted, removing!");
                }
            } else {
                CSYNC_LOG(CSYNC_LOG_PRIORITY_WARN, "sqlite version mismatch");
            }
        }
        _tunlink(wstatedb);
    }

    c_free_multibyte(wstatedb);

    /* create database */
    rc = sqlite3_open(statedb, &ctx->statedb.db);
    sqlite3_close(ctx->statedb.db);
    ctx->statedb.db = 0;

    if (rc == SQLITE_OK) {
        _csync_win32_hide_file(statedb);
        return 1;
    }
    CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "sqlite3_open failed: %s %s", sqlite3_errmsg(ctx->statedb.db), statedb);
    return -1;
}

static int _csync_statedb_is_empty(CSYNC *ctx) {
  c_strlist_t *result = NULL;
  int rc = 0;

  result = csync_statedb_query(ctx, "SELECT COUNT(phash) FROM metadata LIMIT 1 OFFSET 0;");
  if (result == NULL) {
    rc = 1;
  }
  c_strlist_destroy(result);

  return rc;
}

int csync_statedb_load(CSYNC *ctx, const char *statedb) {
  int rc = -1;
  int check_rc = -1;
  c_strlist_t *result = NULL;
  char *statedb_tmp = NULL;

  /* csync_statedb_check tries to open the statedb and creates it in case
   * its not there.
   */
  check_rc = _csync_statedb_check(ctx, statedb);
  if (check_rc < 0) {
    CSYNC_LOG(CSYNC_LOG_PRIORITY_NOTICE, "ERR: checking csync database failed - bail out.");

    rc = -1;
    goto out;
  }

  /*
   * We want a two phase commit for the jounal, so we create a temporary copy
   * of the database.
   * The intention is that if something goes wrong we will not loose the
   * statedb.
   */
  if (asprintf(&statedb_tmp, "%s.ctmp", statedb) < 0) {
    CSYNC_LOG(CSYNC_LOG_PRIORITY_NOTICE, "ERR: could not create statedb name - bail out.");

    rc = -1;
    goto out;
  }

  if (c_copy(statedb, statedb_tmp, 0644) < 0) {
    CSYNC_LOG(CSYNC_LOG_PRIORITY_NOTICE, "ERR: Failed to copy statedb -> statedb_tmp - bail out.");

    rc = -1;
    goto out;
  }

  _csync_win32_hide_file( statedb_tmp );

  /* Open or create the temporary database */
  if (sqlite3_open(statedb_tmp, &ctx->statedb.db) != SQLITE_OK) {
    const char *errmsg= sqlite3_errmsg(ctx->statedb.db);
    CSYNC_LOG(CSYNC_LOG_PRIORITY_NOTICE, "ERR: Failed to sqlite3 open statedb - bail out: %s.",
              errmsg ? errmsg : "<no sqlite3 errormsg>");

    rc = -1;
    goto out;
  }

  /* If check_rc == 1 the database is new and empty as a result. */
  if ((check_rc == 1) || _csync_statedb_is_empty(ctx)) {
    CSYNC_LOG(CSYNC_LOG_PRIORITY_NOTICE, "statedb doesn't exist");
    csync_set_statedb_exists(ctx, 0);
  } else {
    csync_set_statedb_exists(ctx, 1);
  }

  /* optimization for speeding up SQLite */
  result = csync_statedb_query(ctx, "PRAGMA synchronous = FULL;");
  c_strlist_destroy(result);
  result = csync_statedb_query(ctx, "PRAGMA case_sensitive_like = ON;");
  c_strlist_destroy(result);

  rc = 0;
out:
  SAFE_FREE(statedb_tmp);
  return rc;
}

int csync_statedb_write(CSYNC *ctx) {
  bool recreate_db = false;

  /* drop tables */
  if (csync_statedb_drop_tables(ctx) < 0) {
    recreate_db = true;
  }

  /* create tables */
  if (! recreate_db) {
    if (csync_statedb_create_tables(ctx) < 0) {
      recreate_db = true;
    }
  }

  if (recreate_db) {
    char *statedb_tmp;
    _TCHAR *wstatedb_tmp = NULL;

    int rc;
    if (asprintf(&statedb_tmp, "%s.ctmp", ctx->statedb.file) < 0) {
      return -1;
    }
    /* close the temporary database */
    sqlite3_close(ctx->statedb.db);

    /*  remove a possible corrupted file if it exists */
    wstatedb_tmp = c_multibyte(statedb_tmp);
    _tunlink(wstatedb_tmp);
    c_free_multibyte(wstatedb_tmp);

    rc = sqlite3_open(statedb_tmp, &ctx->statedb.db);
    SAFE_FREE(statedb_tmp);
    if (rc != SQLITE_OK) {
      return -1;
    }
    /* create tables */
    if (csync_statedb_create_tables(ctx) < 0) {
        return -1;
    }
  }

  /* insert metadata */
  if (csync_statedb_insert_metadata(ctx) < 0) {
    return -1;
  }

  /* progress info */
  if (csync_statedb_write_progressinfo(ctx, ctx->progress) < 0) {
    return -1;
  }

  return 0;
}

int csync_statedb_close(CSYNC *ctx, const char *statedb, int jwritten) {
  char *statedb_tmp = NULL;
  int rc = 0;
  _TCHAR *wstatedb_tmp = NULL;

  /* close the temporary database */
  rc = sqlite3_close(ctx->statedb.db);
  if( rc == SQLITE_BUSY ) {
      CSYNC_LOG(CSYNC_LOG_PRIORITY_NOTICE, "WARN: sqlite3_close got busy!");
  }

  if (asprintf(&statedb_tmp, "%s.ctmp", statedb) < 0) {
    return -1;
  }

  /* if we successfully synchronized, overwrite the original statedb */
  if (jwritten) {
      rc = c_copy(statedb_tmp, statedb, 0644);
  }

  wstatedb_tmp = c_multibyte(statedb_tmp);
  if (wstatedb_tmp) {
      _tunlink(wstatedb_tmp);
      c_free_multibyte(wstatedb_tmp);
  }

  SAFE_FREE(statedb_tmp);

  return rc;
}

int csync_statedb_create_tables(CSYNC *ctx) {
  c_strlist_t *result = NULL;
  int rc;
  char *stmt;

  /*
   * Create temorary table to work on, this speeds up the
   * creation of the statedb.
   */
  result = csync_statedb_query(ctx,
      "CREATE TABLE IF NOT EXISTS metadata_temp("
      "phash INTEGER(8),"
      "pathlen INTEGER,"
      "path VARCHAR(4096),"
      "inode INTEGER,"
      "uid INTEGER,"
      "gid INTEGER,"
      "mode INTEGER,"
      "modtime INTEGER(8),"
      "type INTEGER,"
      "md5 VARCHAR(32),"
      "PRIMARY KEY(phash)"
      ");"
      );

  if (result == NULL) {
    return -1;
  }
  c_strlist_destroy(result);

  /*
   * Create 'real' table if not existing. That is only important at the
   * first sync so that other functions do not complain about missing
   * tables.
   */
  result = csync_statedb_query(ctx,
      "CREATE TABLE IF NOT EXISTS metadata("
      "phash INTEGER(8),"
      "pathlen INTEGER,"
      "path VARCHAR(4096),"
      "inode INTEGER,"
      "uid INTEGER,"
      "gid INTEGER,"
      "mode INTEGER,"
      "modtime INTEGER(8),"
      "type INTEGER,"
      "md5 VARCHAR(32),"
      "PRIMARY KEY(phash)"
      ");"
      );

  if (result == NULL) {
    return -1;
  }
  c_strlist_destroy(result);

  result = csync_statedb_query(ctx,
                               "CREATE TABLE IF NOT EXISTS version("
                               "major INTEGER(8),"
                               "minor INTEGER(8),"
                               "patch INTEGER(8),"
                               "custom VARCHAR(256));" );
  if (result == NULL) {
    return -1;
  }
  c_strlist_destroy(result);
  
  result = csync_statedb_query(ctx,
                               "CREATE TABLE IF NOT EXISTS progress("
                               "phash INTEGER(8),"
                               "modtime INTEGER(8),"
                               "md5 VARCHAR(32),"
                               "chunk INTEGER(4),"
                               "transferid INTEGER(4),"
                               "error_count INTEGER(8),"
                               "tmpfile VARCHAR(4096),"
                               "error_string VARCHAR(4096),"
                               "PRIMARY KEY(phash)"
                               ");"
  );
  if (result == NULL) {
    return -1;
  }
  c_strlist_destroy(result);
  

  /* write the version table. */
  stmt = sqlite3_mprintf( "INSERT INTO version (major, minor, patch) VALUES (%d, %d, %d);",
                          LIBCSYNC_VERSION_MAJOR, LIBCSYNC_VERSION_MINOR, LIBCSYNC_VERSION_MICRO );

  rc = csync_statedb_insert(ctx, stmt);

  if( rc < 0 ) {
    CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE, "Error: Failed to insert into version table.");
    return -1;
  }
  sqlite3_free(stmt);
  

  return 0;
}

int csync_statedb_drop_tables(CSYNC *ctx) {
  c_strlist_t *result = NULL;

  result = csync_statedb_query(ctx,
      "DROP TABLE IF EXISTS metadata_temp;"
      );
  if (result == NULL) {
    return -1;
  }
  c_strlist_destroy(result);
  
  result = csync_statedb_query(ctx,
      "DROP TABLE IF EXISTS progress;"
      );
  if (result == NULL) {
    return -1;
  }
  c_strlist_destroy(result);

  result = csync_statedb_query(ctx,
      "DROP TABLE IF EXISTS version;"
      );
  if (result == NULL) {
    return -1;
  }
  c_strlist_destroy(result);

  return 0;
}

static int _insert_metadata_visitor(void *obj, void *data) {
  csync_file_stat_t *fs = NULL;
  CSYNC *ctx = NULL;
  sqlite3_stmt *stmt = NULL;
  const char *md5 = "";
  int rc = -1;

  fs = (csync_file_stat_t *) obj;
  ctx = (CSYNC *) data;
  if (ctx == NULL) {
    CSYNC_LOG(CSYNC_LOG_PRIORITY_WARN, "Statement visitor data invalid!");
    return -1;
  }

  stmt = csync_get_userdata(ctx);
  if (stmt == NULL) {
    CSYNC_LOG(CSYNC_LOG_PRIORITY_WARN, "Statement visitor data invalid!");
    return -1;
  }

  switch (fs->instruction) {
    /*
     * Don't write ignored, deleted or files with an error to the statedb.
     * They will be visited on the next synchronization again as a new file.
     */
    case CSYNC_INSTRUCTION_DELETED:
    case CSYNC_INSTRUCTION_IGNORE:
    case CSYNC_INSTRUCTION_ERROR:
      rc = 0;
      break;
    case CSYNC_INSTRUCTION_NONE:
    /* As we only sync the local tree we need this flag here */
    case CSYNC_INSTRUCTION_UPDATED:
  case CSYNC_INSTRUCTION_CONFLICT:
      CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE,
                "SQL statement: INSERT INTO metadata_temp \n"
                "\t\t\t(phash, pathlen, path, inode, uid, gid, mode, modtime, type, md5) VALUES \n"
                "\t\t\t(%lld, %lu, %s, %lld, %u, %u, %u, %lu, %d, %s);",
                (long long signed int) fs->phash,
                (long unsigned int) fs->pathlen,
                fs->path,
                (long long signed int) fs->inode,
                fs->uid,
                fs->gid,
                fs->mode,
                fs->modtime,
                fs->type,
                fs->md5 ? fs->md5 : "<empty>");

      /*
       * The phash needs to be long long unsigned int or it segfaults on PPC
       */
      sqlite3_bind_int64(stmt, 1, (long long signed int) fs->phash);
      sqlite3_bind_int64(stmt, 2, (long unsigned int) fs->pathlen);
      sqlite3_bind_text( stmt, 3, fs->path, fs->pathlen, SQLITE_STATIC);
      sqlite3_bind_int64(stmt, 4, (long long signed int) fs->inode);
      sqlite3_bind_int(  stmt, 5, fs->uid);
      sqlite3_bind_int(  stmt, 6, fs->gid);
      sqlite3_bind_int(  stmt, 7, fs->mode);
      sqlite3_bind_int64(stmt, 8, fs->modtime);
      sqlite3_bind_int(  stmt, 9, fs->type);

      /* The md5 sum might be zero for directories. They will be investigated in the next
       * sync, called "Oliviers patch". */
      if (fs->md5 != NULL) {
          md5 = fs->md5;
      }
      sqlite3_bind_text( stmt,10, md5, strlen(md5), SQLITE_STATIC);

      rc = sqlite3_step(stmt);
      if ( rc != SQLITE_DONE) {
          CSYNC_LOG(CSYNC_LOG_PRIORITY_WARN, "sqlite insert failed: %s", sqlite3_errmsg(ctx->statedb.db));
          rc = -1;
      }

    sqlite3_reset(stmt);

      break;
    default:
      CSYNC_LOG(CSYNC_LOG_PRIORITY_WARN,
          "file: %s, instruction: %s (%d), not added to statedb!",
          fs->path, csync_instruction_str(fs->instruction), fs->instruction);
      rc = 1;
      break;
  }

  return rc;
}

int csync_statedb_insert_metadata(CSYNC *ctx) {
  c_strlist_t *result = NULL;
  char buffer[] = "INSERT INTO metadata_temp VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)";
  sqlite3_stmt* stmt;
  int rc;

  /* start a transaction */
  result = csync_statedb_query(ctx, "BEGIN TRANSACTION;");
  c_strlist_destroy(result);

  /* prepare the INSERT statement */
  rc = sqlite3_prepare_v2(ctx->statedb.db, buffer, strlen(buffer), &stmt, NULL);
  if( rc != SQLITE_OK ) {
    return -1;
  }

  /* and store the insert statement handle to the ctx as userdata. */
  csync_set_userdata(ctx, stmt);

  rc = c_rbtree_walk(ctx->local.tree, ctx, _insert_metadata_visitor);
  sqlite3_finalize( stmt );

  /* Commit the result even if there was an error */
  result = csync_statedb_query(ctx, "COMMIT TRANSACTION;");

  c_strlist_destroy(result);

  /* FIXME: How do we deal with an error in rbtree_walk? No rollback needed actually */
  if (rc < 0) {
    /* We stay with metadata and remove the tmp database */
    result = csync_statedb_query(ctx, "DROP TABLE metadata_temp;");
    c_strlist_destroy(result);

    return -1;
  }

  /* If all goes well, drop metadata and rename metadata_temp */
  result = csync_statedb_query(ctx, "BEGIN TRANSACTION;");
  c_strlist_destroy(result);

  result = csync_statedb_query(ctx, "DROP TABLE IF EXISTS metadata;");
  c_strlist_destroy(result);

  result = csync_statedb_query(ctx, "ALTER TABLE metadata_temp RENAME TO metadata;");
  c_strlist_destroy(result);

  result = csync_statedb_query(ctx,
                               "CREATE INDEX IF NOT EXISTS metadata_phash ON metadata(phash);");
  if (result == NULL) {
      return -1;
  }
  c_strlist_destroy(result);

  result = csync_statedb_query(ctx,
                               "CREATE INDEX IF NOT EXISTS metadata_inode ON metadata(inode);");
  if (result == NULL) {
      return -1;
  }
  c_strlist_destroy(result);


  result = csync_statedb_query(ctx, "COMMIT TRANSACTION;");
  c_strlist_destroy(result);

  return 0;
}

/* caller must free the memory */
csync_file_stat_t *csync_statedb_get_stat_by_hash(CSYNC *ctx, uint64_t phash) {
  csync_file_stat_t *st = NULL;
  c_strlist_t *result = NULL;
  char *stmt = NULL;
  size_t len = 0;

  stmt = sqlite3_mprintf("SELECT * FROM metadata WHERE phash='%lld'",
      (long long signed int) phash);
  if (stmt == NULL) {
    return NULL;
  }

  result = csync_statedb_query(ctx, stmt);
  sqlite3_free(stmt);
  if (result == NULL) {
    return NULL;
  }

  if (result->count != 0 && result->count < 10) {
    CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "WRN: Amount of result columns wrong, db version mismatch!");
  }
  if(result->count > 7) {
      /* phash, pathlen, path, inode, uid, gid, mode, modtime */
      len = strlen(result->vector[2]);
      st = c_malloc(sizeof(csync_file_stat_t) + len + 1);
      if (st == NULL) {
          c_strlist_destroy(result);
          return NULL;
      }
      /* clear the whole structure */
      ZERO_STRUCTP(st);

      /*
   * FIXME:
   * We use an INTEGER(8) which is signed to the phash in the sqlite3 db,
   * but the phash is an uint64_t. So for some values we get a string like
   * "1.66514565505016e+19". For such a string strtoull() returns 1.
   * phash = 1
   *
   * st->phash = strtoull(result->vector[0], NULL, 10);
   */

      /* The query suceeded so use the phash we pass to the function. */
      st->phash = phash;

      st->pathlen = atoi(result->vector[1]);
      memcpy(st->path, (len ? result->vector[2] : ""), len + 1);
      st->inode = atoi(result->vector[3]);
      st->uid = atoi(result->vector[4]);
      st->gid = atoi(result->vector[5]);
      st->mode = atoi(result->vector[6]);
      st->modtime = strtoul(result->vector[7], NULL, 10);

      if(st && result->count > 8 && result->vector[8]) {
          st->type = atoi(result->vector[8]);
      }

      if(result->count > 9 && result->vector[9]) {
          st->md5 = c_strdup( result->vector[9] );
      }
  } else {
      CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE, "No result record found for phash = %llu",
                (long long unsigned int) phash);
      SAFE_FREE(st);
  }

  c_strlist_destroy(result);

  return st;
}

/* caller must free the memory */
csync_file_stat_t *csync_statedb_get_stat_by_inode(CSYNC *ctx, uint64_t inode) {
  csync_file_stat_t *st = NULL;
  c_strlist_t *result = NULL;
  char *stmt = NULL;
  size_t len = 0;

  stmt = sqlite3_mprintf("SELECT * FROM metadata WHERE inode='%lld'",
			 (long long unsigned int) inode);
  if (stmt == NULL) {
    return NULL;
  }

  result = csync_statedb_query(ctx, stmt);
  sqlite3_free(stmt);
  if (result == NULL) {
    return NULL;
  }

  if (result->count <= 6) {
    c_strlist_destroy(result);
    return NULL;
  }

  /* phash, pathlen, path, inode, uid, gid, mode, modtime */
  len = strlen(result->vector[2]);
  st = c_malloc(sizeof(csync_file_stat_t) + len + 1);
  if (st == NULL) {
    c_strlist_destroy(result);
    return NULL;
  }
  /* clear the whole structure */
  ZERO_STRUCTP(st);

  st->phash = strtoull(result->vector[0], NULL, 10);
  st->pathlen = atoi(result->vector[1]);
  memcpy(st->path, (len ? result->vector[2] : ""), len + 1);
  st->inode = atoi(result->vector[3]);
  st->uid = atoi(result->vector[4]);
  st->gid = atoi(result->vector[5]);
  st->mode = atoi(result->vector[6]);
  st->modtime = strtoul(result->vector[7], NULL, 10);
  st->type = atoi(result->vector[8]);
  if( result->vector[9] )
    st->md5 = c_strdup(result->vector[9]);

  c_strlist_destroy(result);

  return st;
}

char *csync_statedb_get_uniqId( CSYNC *ctx, uint64_t jHash, csync_vio_file_stat_t *buf ) {
    char *ret = NULL;
    c_strlist_t *result = NULL;
    char *stmt = NULL;
    (void)buf;

    if( ! csync_get_statedb_exists(ctx)) return ret;

    stmt = sqlite3_mprintf("SELECT md5 FROM metadata WHERE phash='%lld'", jHash);

    result = csync_statedb_query(ctx, stmt);
    sqlite3_free(stmt);
    if (result == NULL) {
      return NULL;
    }

    if (result->count == 1) {
        /* phash, pathlen, path, inode, uid, gid, mode, modtime */
        ret = c_strdup( result->vector[0] );
    }

    c_strlist_destroy(result);

    return ret;
}

c_strlist_t *csync_statedb_get_below_path( CSYNC *ctx, const char *path ) {
    c_strlist_t *list = NULL;
    char *stmt = NULL;

    stmt = sqlite3_mprintf("SELECT phash, path, inode, uid, gid, mode, modtime, type, md5 "
                           "FROM metadata WHERE path LIKE('%q/%%')", path);
    if (stmt == NULL) {
      return NULL;
    }

    CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG, "SQL: %s", stmt);

    list = csync_statedb_query( ctx, stmt );

    sqlite3_free(stmt);

    return list;
}


/* query the statedb, caller must free the memory */
c_strlist_t *csync_statedb_query(CSYNC *ctx, const char *statement) {
  int err = SQLITE_OK;
  int rc = SQLITE_OK;
  size_t i = 0;
  size_t busy_count = 0;
  size_t retry_count = 0;
  size_t column_count = 0;
  sqlite3_stmt *stmt;
  const char *tail = NULL;
  c_strlist_t *result = NULL;
  int row = 0;

  do {
    /* compile SQL program into a virtual machine, reattempteing if busy */
    do {
      if (busy_count) {
        /* sleep 100 msec */
        usleep(100000);
        CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG, "sqlite3_prepare: BUSY counter: %zu", busy_count);
      }
      err = sqlite3_prepare(ctx->statedb.db, statement, -1, &stmt, &tail);
    } while (err == SQLITE_BUSY && busy_count ++ < 120);

    if (err != SQLITE_OK) {
      if (err == SQLITE_BUSY) {
        CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "Gave up waiting for lock to clear");
      }
      CSYNC_LOG(CSYNC_LOG_PRIORITY_WARN, "sqlite3_compile error: %s - on query %s", sqlite3_errmsg(ctx->statedb.db), statement);
      break;
    } else {
      busy_count = 0;
      column_count = sqlite3_column_count(stmt);

      /* execute virtual machine by iterating over rows */
      for(;;) {
        err = sqlite3_step(stmt);

        if (err == SQLITE_BUSY) {
          if (busy_count++ > 120) {
            CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "Busy counter has reached its maximum. Aborting this sql statement");
            break;
          }
          /* sleep 100 msec */
          usleep(100000);
          CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE, "sqlite3_step: BUSY counter: %zu", busy_count);
          continue;
        }

        if (err == SQLITE_MISUSE) {
          CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "sqlite3_step: MISUSE!!");
        }

        if (err == SQLITE_DONE) {
          if (result == NULL) {
            result = c_strlist_new(1);
          }
          break;
        }

        if (err == SQLITE_ERROR) {
          break;
        }

       row++;
        if( result ) {
            result = c_strlist_expand(result, row*column_count);
        } else {
            result = c_strlist_new(column_count);
        }

        if (result == NULL) {
          return NULL;
        }

        /* iterate over columns */
        for (i = 0; i < column_count; i++) {
          // CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE, "sqlite3_column_text: %s", (char *) sqlite3_column_text(stmt, i));
          if (c_strlist_add(result, (char *) sqlite3_column_text(stmt, i)) < 0) {
            c_strlist_destroy(result);
            return NULL;
          }
        }
      } /* end infinite for loop */

      /* deallocate vm resources */
      rc = sqlite3_finalize(stmt);

      if (err != SQLITE_DONE && rc != SQLITE_SCHEMA) {
        CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "sqlite_step error: %s - on query: %s", sqlite3_errmsg(ctx->statedb.db), statement);
        if (result != NULL) {
          c_strlist_destroy(result);
        }
        return NULL;
      }

      if (rc == SQLITE_SCHEMA) {
        retry_count ++;
        CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "SQLITE_SCHEMA error occurred on query: %s", statement);
        if (retry_count < 10) {
          CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG, "Retrying now.");
        } else {
          CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "RETRY count has reached its maximum. Aborting statement: %s", statement);
          if (result != NULL) {
            c_strlist_destroy(result);
          }
          result = c_strlist_new(1);
        }
      }
    }
  } while (rc == SQLITE_SCHEMA && retry_count < 10);

  return result;
}

int csync_statedb_insert(CSYNC *ctx, const char *statement) {
  int err;
  int rc = 0;
  int busy_count = 0;
  int retry_count = 0;
  sqlite3_stmt *stmt;
  const char *tail;

  if (!statement[0]) {
    return 0;
  }

  do {
    /* compile SQL program into a virtual machine, reattempteing if busy */
    do {
      if (busy_count) {
        /* sleep 100 msec */
        usleep(100000);
        CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG, "sqlite3_prepare: BUSY counter: %d", busy_count);
      }
      err = sqlite3_prepare(ctx->statedb.db, statement, -1, &stmt, &tail);
    } while (err == SQLITE_BUSY && busy_count++ < 120);

    if (err != SQLITE_OK) {
      if (err == SQLITE_BUSY) {
        CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "Gave up waiting for lock to clear");
      }
      CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "sqlite3_compile error: %s on query %s", sqlite3_errmsg(ctx->statedb.db), statement);
      break;
    } else {
      busy_count = 0;

      /* execute virtual machine by iterating over rows */
      for(;;) {
        err = sqlite3_step(stmt);

        if (err == SQLITE_BUSY) {
          if (busy_count++ > 120) {
            CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "Busy counter has reached its maximum. Aborting this sql statement");
            break;
          }
          /* sleep 100 msec */
          usleep(100000);
          CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE, "sqlite3_step: BUSY counter: %d", busy_count);
        }

        if (err == SQLITE_MISUSE) {
          CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "sqlite3_step: MISUSE!!");
        }

        if (err == SQLITE_DONE || err == SQLITE_ERROR) {
          break;
        }
      } /* end infinite for loop */

      /* deallocate vm resources */
      rc = sqlite3_finalize(stmt);

      if (err != SQLITE_DONE && rc != SQLITE_SCHEMA) {
        CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "sqlite_step error: %s on insert: %s", sqlite3_errmsg(ctx->statedb.db), statement);
      }

      if (rc == SQLITE_SCHEMA) {
        retry_count++;
        CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "SQLITE_SCHEMA error occurred on insert: %s", statement);
        if (retry_count < 10) {
          CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG, "Retrying now.");
        } else {
          CSYNC_LOG(CSYNC_LOG_PRIORITY_ERROR, "RETRY count has reached its maximum. Aborting statement: %s", statement);
        }
      }
    }
  } while (rc == SQLITE_SCHEMA && retry_count < 10);

  return sqlite3_last_insert_rowid(ctx->statedb.db);
}

csync_progressinfo_t* csync_statedb_get_progressinfo(CSYNC *ctx, uint64_t phash, uint64_t modtime, const char* md5) {
  char *stmt = NULL;
  csync_progressinfo_t *ret = NULL;
  c_strlist_t *result = NULL;

  if( ! csync_get_statedb_exists(ctx)) return ret;
  stmt = sqlite3_mprintf("SELECT error_count, chunk, transferid, tmpfile, error_string FROM progress WHERE phash='%llu' AND modtime='%lld' AND md5='%q'",
                         (long long unsigned int) phash, (long long signed int) modtime, md5);
  if (!stmt) return ret;

  result = csync_statedb_query(ctx, stmt);
  sqlite3_free(stmt);
  if (result == NULL) {
    return NULL;
  }

  if (result->count == 5) {
    ret = c_malloc(sizeof(csync_progressinfo_t));
    if (!ret) goto out;
    ret->next = NULL;
    ret->chunk = atoi(result->vector[1]);
    ret->error = atoi(result->vector[0]);
    ret->transferId = atoi(result->vector[2]);
    ret->tmpfile = c_strdup(result->vector[3]);
    ret->md5 = md5 ? c_strdup(md5) : NULL;
    ret->modtime = modtime;
    ret->phash = phash;
    ret->error_string = c_strdup(result->vector[4]);
  }
out:
  c_strlist_destroy(result);
  return ret;
}

void csync_statedb_free_progressinfo(csync_progressinfo_t* pi)
{
  if (!pi) return;
  SAFE_FREE(pi->md5);
  SAFE_FREE(pi->tmpfile);
  SAFE_FREE(pi->error_string);
  SAFE_FREE(pi);
}

int csync_statedb_write_progressinfo(CSYNC* ctx, csync_progressinfo_t* pi)
{
  int rc = 0;
  char *stmt = NULL;

  while (rc > -1 && pi) {
    stmt = sqlite3_mprintf("INSERT INTO progress "
    "(phash, modtime, md5, chunk, transferid, error_count, tmpfile, error_string) VALUES"
    "(%llu, %lld, '%q', %d, %d, %d, '%q', '%q');",
      (long long signed int) pi->phash,
      (long long int) pi->modtime,
      pi->md5,
      pi->chunk,
      pi->transferId,
      pi->error,
      pi->tmpfile,
      pi->error_string
    );

    if (stmt == NULL) {
      return -1;
    }

    CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE, "%s" , stmt);

    rc = csync_statedb_insert(ctx, stmt);
    sqlite3_free(stmt);
    pi = pi->next;
  }
  return 0;
}



/* vim: set ts=8 sw=2 et cindent: */
