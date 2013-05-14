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

#include "csync_private.h"
#include "csync_reconcile.h"
#include "csync_util.h"
#include "csync_statedb.h"
#include "csync_rename.h"
#include "c_jhash.h"

#define CSYNC_LOG_CATEGORY_NAME "csync.reconciler"
#include "csync_log.h"

#define ACCEPTED_TIME_DIFF 5
#define ONE_HOUR 3600

/*
 * We merge replicas at the file level. The merged replica contains the
 * superset of files that are on the local machine and server copies of
 * the replica. In the case where the same file is in both the local
 * and server copy, the file that was modified most recently is used.
 * This means that new files are not deleted, and updated versions of
 * existing files are not overwritten.
 *
 * When a file is updated, the merge algorithm compares the destination
 * file with the the source file. If the destination file is newer
 * (timestamp is newer), it is not overwritten. If both files, on the
 * source and the destination, have been changed, the newer file wins.
 */
static int _csync_merge_algorithm_visitor(void *obj, void *data) {
    csync_file_stat_t *cur = NULL;
    csync_file_stat_t *other = NULL;
    csync_file_stat_t *tmp = NULL;
    uint64_t h = 0;
    int len = 0;

    CSYNC *ctx = NULL;
    c_rbtree_t *tree = NULL;
    c_rbnode_t *node = NULL;

    cur = (csync_file_stat_t *) obj;
    ctx = (CSYNC *) data;

    /* we need the opposite tree! */
    switch (ctx->current) {
    case LOCAL_REPLICA:
        tree = ctx->remote.tree;
        break;
    case REMOTE_REPLICA:
        tree = ctx->local.tree;
        break;
    default:
        break;
    }

    node = c_rbtree_find(tree, &cur->phash);

    if (!node && ctx->current == REMOTE_REPLICA) {
        /* Check the renamed path as well. */
        char *renamed_path = csync_rename_adjust_path(ctx, cur->path);
        if (!c_streq(renamed_path, cur->path)) {
            len = strlen( renamed_path );
            h = c_jhash64((uint8_t *) renamed_path, len, 0);
            node = c_rbtree_find(tree, &h);
        }
        SAFE_FREE(renamed_path);
    }

    /* file only found on current replica */
    if (node == NULL) {
        switch(cur->instruction) {
        /* file has been modified */
        case CSYNC_INSTRUCTION_EVAL:
            cur->instruction = CSYNC_INSTRUCTION_NEW;
            break;
            /* file has been removed on the opposite replica */
        case CSYNC_INSTRUCTION_NONE:
            cur->instruction = CSYNC_INSTRUCTION_REMOVE;
            break;
        case CSYNC_INSTRUCTION_RENAME:
            /* rename support only on the local replica because of inode needed. */
            if(ctx->current == LOCAL_REPLICA ) {
                /* use the old name to find the "other" node */
                tmp = csync_statedb_get_stat_by_inode(ctx, cur->inode);
                /* Find the opposite node. */
                if( tmp ) {
                    /* We need to calculate the phash again because of the phash being stored as int in db. */
                    if( tmp->path ) {
                        len = strlen( tmp->path );
                        h = c_jhash64((uint8_t *) tmp->path, len, 0);

                        CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE,"PHash of temporar opposite: %ld", h);
                        node = c_rbtree_find(tree, &h);
                    }
                    if(node) {
                        other = (csync_file_stat_t*)node->data;
                    }
                    if(!other) {
                        cur->instruction = CSYNC_INSTRUCTION_NEW;
                    } else if (other->instruction == CSYNC_INSTRUCTION_NONE
                                || cur->type == CSYNC_FTW_TYPE_DIR) {
                        other->instruction = CSYNC_INSTRUCTION_RENAME;
                        other->destpath = c_strdup( cur->path );
                        cur->instruction = CSYNC_INSTRUCTION_NONE;
                    } else {
                        cur->instruction = CSYNC_INSTRUCTION_NONE;
                        other->instruction = CSYNC_INSTRUCTION_SYNC;
                    }

                    SAFE_FREE(tmp->md5);
                    SAFE_FREE(tmp);
                }
            }
            break;
        default:
            break;
        }
    } else {
        /*
     * file found on the other replica
     */
        other = (csync_file_stat_t *) node->data;

        switch (cur->instruction) {
        case CSYNC_INSTRUCTION_RENAME:
            if(ctx->current != LOCAL_REPLICA )
                break;
            /* If the file already exist on the other side, we have a conflict.
               Abort the rename and consider it is a new file. */
            cur->instruction = CSYNC_INSTRUCTION_NEW;
            /* fall trough */
        /* file on current replica is changed or new */
        case CSYNC_INSTRUCTION_EVAL:
        case CSYNC_INSTRUCTION_NEW:
            switch (other->instruction) {
            /* file on other replica is changed or new */
            case CSYNC_INSTRUCTION_NEW:
            case CSYNC_INSTRUCTION_EVAL:
                if (other->size == cur->size &&  other->modtime == cur->modtime) {
                    /* The files are considered equal. */
                    cur->instruction = CSYNC_INSTRUCTION_NONE;
                    other->instruction = CSYNC_INSTRUCTION_NONE;

                    if( !cur->md5 && other->md5 ) cur->md5 = c_strdup(other->md5);
                } else if(ctx->current == REMOTE_REPLICA) {
                    if(ctx->options.with_conflict_copys) {
                        cur->instruction = CSYNC_INSTRUCTION_CONFLICT;
                        other->instruction = CSYNC_INSTRUCTION_NONE;
                    } else {
                        cur->instruction = CSYNC_INSTRUCTION_SYNC;
                        other->instruction = CSYNC_INSTRUCTION_NONE;
                    }
                } else {
                    if(ctx->options.with_conflict_copys) {
                        cur->instruction = CSYNC_INSTRUCTION_NONE;
                        other->instruction = CSYNC_INSTRUCTION_CONFLICT;
                    } else {
                        cur->instruction = CSYNC_INSTRUCTION_NONE;
                        other->instruction = CSYNC_INSTRUCTION_SYNC;
                    }
                }

                break;
                /* file on the other replica has not been modified */
            case CSYNC_INSTRUCTION_NONE:
                cur->instruction = CSYNC_INSTRUCTION_SYNC;
                break;
            case CSYNC_INSTRUCTION_IGNORE:
                cur->instruction = CSYNC_INSTRUCTION_IGNORE;
            break;
            default:
                break;
            }
        default:
            break;
        }
    }

    //hide instruction NONE messages when log level is set to debug,
    //only show these messages on log level trace
    if(cur->instruction ==CSYNC_INSTRUCTION_NONE)
    {
        if(cur->type == CSYNC_FTW_TYPE_DIR)
        {
            CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE,
                      "%-20s  dir: %s",
                      csync_instruction_str(cur->instruction),
                      cur->path);
        }
        else
        {
            CSYNC_LOG(CSYNC_LOG_PRIORITY_TRACE,
                      "%-20s file: %s",
                      csync_instruction_str(cur->instruction),
                      cur->path);
        }
    }
    else
    {
        if(cur->type == CSYNC_FTW_TYPE_DIR)
        {
            CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG,
                      "%-20s  dir: %s",
                      csync_instruction_str(cur->instruction),
                      cur->path);
        }
        else
        {
            CSYNC_LOG(CSYNC_LOG_PRIORITY_DEBUG,
                      "%-20s file: %s",
                      csync_instruction_str(cur->instruction),
                      cur->path);
        }
    }

    return 0;
}

int csync_reconcile_updates(CSYNC *ctx) {
  int rc;
  c_rbtree_t *tree = NULL;

  switch (ctx->current) {
    case LOCAL_REPLICA:
      tree = ctx->local.tree;
      break;
    case REMOTE_REPLICA:
      tree = ctx->remote.tree;
      break;
    default:
      break;
  }

  rc = c_rbtree_walk(tree, (void *) ctx, _csync_merge_algorithm_visitor);

  return rc;
}

/* vim: set ts=8 sw=2 et cindent: */
