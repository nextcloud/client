/*
 * libcsync -- a library to sync a directory with another
 *
 * Copyright (c) 2008-2013 by Andreas Schneider <asn@cryptomilk.org>
 * Copyright (c) 2012-2013 by Klaas Freitag <freitag@owncloud.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config_csync.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "c_lib.h"

#include "csync_private.h"
#include "csync_exclude.h"
#include "csync_update.h"
#include "csync_util.h"
#include "csync_misc.h"

#include "vio/csync_vio.h"

#include "csync_rename.h"

#include "common/utility.h"
#include "common/asserts.h"

#include <QtCore/QTextCodec>

// Needed for PRIu64 on MinGW in C++ mode.
#define __STDC_FORMAT_MACROS
#include <inttypes.h>

Q_LOGGING_CATEGORY(lcUpdate, "nextcloud.sync.csync.updater", QtInfoMsg)

#ifdef NO_RENAME_EXTENSION
/* Return true if the two path have the same extension. false otherwise. */
static bool _csync_sameextension(const char *p1, const char *p2) {
    /* Find pointer to the extensions */
    const char *e1 = strrchr(p1, '.');
    const char *e2 = strrchr(p2, '.');

    /* If the found extension contains a '/', it is because the . was in the folder name
     *            => no extensions */
    if (e1 && strchr(e1, '/')) e1 = NULL;
    if (e2 && strchr(e2, '/')) e2 = NULL;

    /* If none have extension, it is the same extension */
    if (!e1 && !e2)
        return true;

    /* c_streq takes care of the rest */
    return c_streq(e1, e2);
}
#endif

static QByteArray _rel_to_abs(CSYNC* ctx, const QByteArray &relativePath) {
    return QByteArray() % const_cast<const char *>(ctx->local.uri) % '/' % relativePath;
}

/* Return true if two mtime are considered equal
 * We consider mtime that are one hour difference to be equal if they are one hour appart
 * because on some system (FAT) the date is changing when the daylight saving is changing */
static bool _csync_mtime_equal(time_t a, time_t b)
{
    if (a == b)
        return true;

    /* 1h of difference +- 1 second because the accuracy of FAT is 2 seconds (#2438) */
    if (fabs(3600 - fabs(difftime(a, b))) < 2)
        return true;

    return false;
}

/**
 * The main function of the discovery/update pass.
 *
 * It's called (indirectly) by csync_update(), once for each entity in the
 * local filesystem and once for each entity in the server data.
 *
 * It has two main jobs:
 * - figure out whether anything happened compared to the sync journal
 *   and set (primarily) the instruction flag accordingly
 * - build the ctx->local.tree / ctx->remote.tree
 *
 * See doc/dev/sync-algorithm.md for an overview.
 */
static int _csync_detect_update(CSYNC *ctx, std::unique_ptr<csync_file_stat_t> fs) {
  Q_ASSERT(fs);
  OCC::SyncJournalFileRecord base;
  CSYNC_EXCLUDE_TYPE excluded = CSYNC_NOT_EXCLUDED;
  if (fs->type == ItemTypeSkip) {
      excluded =CSYNC_FILE_EXCLUDE_STAT_FAILED;
  } else {
      /* Check if file is excluded */
      if (ctx->exclude_traversal_fn)
          excluded = ctx->exclude_traversal_fn(fs->path, fs->type);
  }

  if( excluded == CSYNC_NOT_EXCLUDED ) {
      /* Even if it is not excluded by a pattern, maybe it is to be ignored
       * because it's a hidden file that should not be synced.
       * This code should probably be in csync_exclude, but it does not have the fs parameter.
       * Keep it here for now */
      if (ctx->ignore_hidden_files
          && fs->is_hidden
          && !fs->path.endsWith(".sync-exclude.lst")) {
          qCInfo(lcUpdate, "file excluded because it is a hidden file: %s", fs->path.constData());
          excluded = CSYNC_FILE_EXCLUDE_HIDDEN;
      }
  } else {
      /* File is ignored because it's matched by a user- or system exclude pattern. */
      qCInfo(lcUpdate, "%s excluded  (%d)", fs->path.constData(), excluded);
      if (excluded == CSYNC_FILE_EXCLUDE_AND_REMOVE) {
          return 1;
      }
      if (excluded == CSYNC_FILE_SILENTLY_EXCLUDED) {
          return 1;
      }
  }

  if (ctx->current == REMOTE_REPLICA && ctx->callbacks.checkSelectiveSyncBlackListHook) {
      if (ctx->callbacks.checkSelectiveSyncBlackListHook(ctx->callbacks.update_callback_userdata, fs->path)) {
          return 1;
      }
  }

  auto localCodec = QTextCodec::codecForLocale();
  if (ctx->current == REMOTE_REPLICA && localCodec->mibEnum() != 106) {
      /* If the locale codec is not UTF-8, we must check that the filename from the server can
       * be encoded in the local file system.
       *
       * We cannot use QTextCodec::canEncode() since that can incorrectly return true, see
       * https://bugreports.qt.io/browse/QTBUG-6925.
       */
      QTextEncoder encoder(localCodec, QTextCodec::ConvertInvalidToNull);
      if (encoder.fromUnicode(QString::fromUtf8(fs->path)).contains('\0')) {
          qCInfo(lcUpdate, "cannot encode %s to local encoding %d",
              fs->path.constData(), localCodec->mibEnum());
          excluded = CSYNC_FILE_EXCLUDE_CANNOT_ENCODE;
      }
  }

  if (fs->type == ItemTypeFile ) {
    if (fs->modtime == 0) {
      qCInfo(lcUpdate, "file: %s - mtime is zero!", fs->path.constData());
    }
  }

  if (excluded > CSYNC_NOT_EXCLUDED || fs->type == ItemTypeSoftLink) {
      fs->instruction = CSYNC_INSTRUCTION_IGNORE;
      if (ctx->current_fs) {
          ctx->current_fs->has_ignored_files = true;
      }

      goto out;
  }

  /* Update detection: Check if a database entry exists.
   * If not, the file is either new or has been renamed. To see if it is
   * renamed, the db gets queried by the inode of the file as that one
   * does not change on rename.
   */
  if(!ctx->statedb->getFileRecord(fs->path, &base)) {
      ctx->status_code = CSYNC_STATUS_UNSUCCESSFUL;
      return -1;
  }


  /*
   * When file is encrypted it's phash (path hash) will not match the local file phash,
   * we could match the e2eMangledName but that might be slow wihout index, and it's
   * not UNIQUE at the moment.
   */
  if (!base.isValid()) {
      if(!ctx->statedb->getFileRecordByE2eMangledName(fs->path, &base)) {
          ctx->status_code = CSYNC_STATUS_UNSUCCESSFUL;
          return -1;
      }
  }

  if(base.isValid()) { /* there is an entry in the database */
      // When the file is loaded from the file system it misses
      // the e2e mangled name
      if (fs->e2eMangledName.isEmpty() && !base._e2eMangledName.isEmpty()) {
          fs->e2eMangledName = base._e2eMangledName;
          fs->path = base._path;
      }

      /* we have an update! */
      qCInfo(lcUpdate, "Database entry found for %s, compare: %" PRId64 " <-> %" PRId64
                                          ", etag: %s <-> %s, inode: %" PRId64 " <-> %" PRId64
                                          ", size: %" PRId64 " <-> %" PRId64 ", perms: %x <-> %x"
                                          ", checksum: %s <-> %s , ignore: %d,  e2e: %s",
                base._path.constData(), ((int64_t) fs->modtime), ((int64_t) base._modtime),
                fs->etag.constData(), base._etag.constData(), (uint64_t) fs->inode, (uint64_t) base._inode,
                (uint64_t) fs->size, (uint64_t) base._fileSize, *reinterpret_cast<short*>(&fs->remotePerm), *reinterpret_cast<short*>(&base._remotePerm),
                fs->checksumHeader.constData(), base._checksumHeader.constData(), base._serverHasIgnoredFiles, base._e2eMangledName.constData());
      if (ctx->current == REMOTE_REPLICA && fs->etag != base._etag) {
          fs->instruction = CSYNC_INSTRUCTION_EVAL;

          // Preserve the EVAL flag later on if the type has changed.
          if (base._type != fs->type) {
              fs->child_modified = true;
          }

          goto out;
      }
      if (ctx->current == LOCAL_REPLICA &&
              (!_csync_mtime_equal(fs->modtime, base._modtime)
               // zero size in statedb can happen during migration
               || (base._fileSize != 0 && fs->size != base._fileSize))) {

          // Checksum comparison at this stage is only enabled for .eml files,
          // check #4754 #4755
          bool isEmlFile = csync_fnmatch("*.eml", fs->path, FNM_CASEFOLD) == 0;
          if (isEmlFile && fs->size == base._fileSize && !base._checksumHeader.isEmpty()) {
              if (ctx->callbacks.checksum_hook) {
                  fs->checksumHeader = ctx->callbacks.checksum_hook(
                      _rel_to_abs(ctx, fs->path), base._checksumHeader,
                      ctx->callbacks.checksum_userdata);
              }
              bool checksumIdentical = false;
              if (!fs->checksumHeader.isEmpty()) {
                  checksumIdentical = fs->checksumHeader == base._checksumHeader;
              }
              if (checksumIdentical) {
                  qCInfo(lcUpdate, "NOTE: Checksums are identical, file did not actually change: %s", fs->path.constData());
                  fs->instruction = CSYNC_INSTRUCTION_UPDATE_METADATA;
                  goto out;
              }
          }

          // Preserve the EVAL flag later on if the type has changed.
          if (base._type != fs->type) {
              fs->child_modified = true;
          }

          fs->instruction = CSYNC_INSTRUCTION_EVAL;
          goto out;
      }
      bool metadata_differ = (ctx->current == REMOTE_REPLICA && (fs->file_id != base._fileId
                                                          || fs->remotePerm != base._remotePerm))
                           || (ctx->current == LOCAL_REPLICA && fs->inode != base._inode);
      if (fs->type == ItemTypeDirectory && ctx->current == REMOTE_REPLICA
              && !metadata_differ && ctx->read_remote_from_db) {
          /* If both etag and file id are equal for a directory, read all contents from
           * the database.
           * The metadata comparison ensure that we fetch all the file id or permission when
           * upgrading owncloud
           */
          qCInfo(lcUpdate, "Reading from database: %s", fs->path.constData());
          ctx->remote.read_from_db = true;
      }
      /* If it was remembered in the db that the remote dir has ignored files, store
       * that so that the reconciler can make advantage of.
       */
      if( ctx->current == REMOTE_REPLICA ) {
          fs->has_ignored_files = base._serverHasIgnoredFiles;
      }
      if (metadata_differ) {
          /* file id or permissions has changed. Which means we need to update them in the DB. */
          qCInfo(lcUpdate, "Need to update metadata for: %s", fs->path.constData());
          fs->instruction = CSYNC_INSTRUCTION_UPDATE_METADATA;
      } else {
          fs->instruction = CSYNC_INSTRUCTION_NONE;
      }
  } else {
      /* check if it's a file and has been renamed */
      if (ctx->current == LOCAL_REPLICA) {
          qCInfo(lcUpdate, "Checking for rename based on inode # %" PRId64 "", (uint64_t) fs->inode);

          OCC::SyncJournalFileRecord base;
          if(!ctx->statedb->getFileRecordByInode(fs->inode, &base)) {
              ctx->status_code = CSYNC_STATUS_UNSUCCESSFUL;
              return -1;
          }

          // Default to NEW unless we're sure it's a rename.
          fs->instruction = CSYNC_INSTRUCTION_NEW;

          bool isRename =
              base.isValid() && base._type == fs->type
                  && ((base._modtime == fs->modtime && base._fileSize == fs->size) || fs->type == ItemTypeDirectory)
#ifdef NO_RENAME_EXTENSION
                  && _csync_sameextension(base._path, fs->path)
#endif
              ;


          // Verify the checksum where possible
          if (isRename && !base._checksumHeader.isEmpty() && ctx->callbacks.checksum_hook
              && fs->type == ItemTypeFile) {
                  fs->checksumHeader = ctx->callbacks.checksum_hook(
                      _rel_to_abs(ctx, fs->path), base._checksumHeader,
                      ctx->callbacks.checksum_userdata);
              if (!fs->checksumHeader.isEmpty()) {
                  qCInfo(lcUpdate, "checking checksum of potential rename %s %s <-> %s", fs->path.constData(), fs->checksumHeader.constData(), base._checksumHeader.constData());
                  isRename = fs->checksumHeader == base._checksumHeader;
              }
          }

          if (isRename) {
              qCInfo(lcUpdate, "pot rename detected based on inode # %" PRId64 "", (uint64_t) fs->inode);
              /* inode found so the file has been renamed */
              fs->instruction = CSYNC_INSTRUCTION_EVAL_RENAME;
              if (fs->type == ItemTypeDirectory) {
                  csync_rename_record(ctx, base._path, fs->path);
              }
          }
          goto out;

      } else {
          qCInfo(lcUpdate, "Checking for rename based on fileid %s", fs->file_id.constData());

          /* Remote Replica Rename check */
          fs->instruction = CSYNC_INSTRUCTION_NEW;

          bool done = false;
          auto renameCandidateProcessing = [&](const OCC::SyncJournalFileRecord &base) {
              if (done)
                  return;
              if (!base.isValid())
                  return;

              // Some things prohibit rename detection entirely.
              // Since we don't do the same checks again in reconcile, we can't
              // just skip the candidate, but have to give up completely.
              if (base._type != fs->type) {
                  qCWarning(lcUpdate, "file types different, not a rename");
                  done = true;
                  return;
              }
              if (fs->type != ItemTypeDirectory && base._etag != fs->etag) {
                  /* File with different etag, don't do a rename, but download the file again */
                  qCWarning(lcUpdate, "file etag different, not a rename");
                  done = true;
                  return;
              }

              // Record directory renames
              if (fs->type == ItemTypeDirectory) {
                  // If the same folder was already renamed by a different entry,
                  // skip to the next candidate
                  if (ctx->renames.folder_renamed_to.count(base._path) > 0) {
                      qCWarning(lcUpdate, "folder already has a rename entry, skipping");
                      return;
                  }
                  csync_rename_record(ctx, base._path, fs->path);
              }

              /* A remote rename can also mean Encryption Mangled Name.
               * if we find one of those in the database, we ignore it.
               */
              if (!base._e2eMangledName.isEmpty()) {
                  qCWarning(lcUpdate, "Encrypted file can not rename");
                  done = true;
                  return;
              }

              qCInfo(lcUpdate, "remote rename detected based on fileid %s --> %s", base._path.constData(), fs->path.constData());

              fs->instruction = CSYNC_INSTRUCTION_EVAL_RENAME;
              done = true;
          };

          if (!ctx->statedb->getFileRecordsByFileId(fs->file_id, renameCandidateProcessing)) {
              ctx->status_code = CSYNC_STATUS_UNSUCCESSFUL;
              return -1;
          }

          if (fs->instruction == CSYNC_INSTRUCTION_NEW
              && ctx->current == REMOTE_REPLICA
              && ctx->callbacks.checkSelectiveSyncNewObjectHook) {
              if (ctx->callbacks.checkSelectiveSyncNewObjectHook(ctx->callbacks.update_callback_userdata, fs->path, fs->remotePerm, (fs->type == ItemTypeDirectory ? true : false))) {
                  return 1;
              }
          }
          goto out;
      }
  }

out:

  /* Set the ignored error string. */
  if (fs->instruction == CSYNC_INSTRUCTION_IGNORE) {
      if( fs->type == ItemTypeSoftLink ) {
          fs->error_status = CSYNC_STATUS_INDIVIDUAL_IS_SYMLINK; /* Symbolic links are ignored. */
      } else {
          if (excluded == CSYNC_FILE_EXCLUDE_LIST) {
              fs->error_status = CSYNC_STATUS_INDIVIDUAL_IGNORE_LIST; /* File listed on ignore list. */
          } else if (excluded == CSYNC_FILE_EXCLUDE_INVALID_CHAR) {
              fs->error_status = CSYNC_STATUS_INDIVIDUAL_IS_INVALID_CHARS;  /* File contains invalid characters. */
          } else if (excluded == CSYNC_FILE_EXCLUDE_TRAILING_SPACE) {
              fs->error_status = CSYNC_STATUS_INDIVIDUAL_TRAILING_SPACE; /* File ends with a trailing space. */
          } else if (excluded == CSYNC_FILE_EXCLUDE_LONG_FILENAME) {
              fs->error_status = CSYNC_STATUS_INDIVIDUAL_EXCLUDE_LONG_FILENAME; /* File name is too long. */
          } else if (excluded == CSYNC_FILE_EXCLUDE_HIDDEN ) {
              fs->error_status = CSYNC_STATUS_INDIVIDUAL_EXCLUDE_HIDDEN;
          } else if (excluded == CSYNC_FILE_EXCLUDE_STAT_FAILED) {
              fs->error_status = CSYNC_STATUS_INDIVIDUAL_STAT_FAILED;
          } else if (excluded == CSYNC_FILE_EXCLUDE_CONFLICT) {
              fs->error_status = CSYNC_STATUS_INDIVIDUAL_IS_CONFLICT_FILE;
          } else if (excluded == CSYNC_FILE_EXCLUDE_CANNOT_ENCODE) {
              fs->error_status = CSYNC_STATUS_INDIVIDUAL_CANNOT_ENCODE;
          }
      }
  }
  if (fs->instruction != CSYNC_INSTRUCTION_NONE
      && fs->instruction != CSYNC_INSTRUCTION_IGNORE
      && fs->instruction != CSYNC_INSTRUCTION_UPDATE_METADATA
      && fs->type != ItemTypeDirectory) {
    fs->child_modified = true;
  }

  // If conflict files are uploaded, they won't be marked as IGNORE / CSYNC_FILE_EXCLUDE_CONFLICT
  // but we still want them marked!
  if (ctx->upload_conflict_files) {
      if (OCC::Utility::isConflictFile(fs->path.constData())) {
          fs->error_status = CSYNC_STATUS_INDIVIDUAL_IS_CONFLICT_FILE;
      }
  }

  ctx->current_fs = fs.get();

  qCInfo(lcUpdate, "file: %s, instruction: %s <<=", fs->path.constData(),
      csync_instruction_str(fs->instruction));

  QByteArray path = fs->path;
  switch (ctx->current) {
    case LOCAL_REPLICA:
      ctx->local.files[path] = std::move(fs);
      break;
    case REMOTE_REPLICA:
      ctx->remote.files[path] = std::move(fs);
      break;
    default:
      break;
  }

  return 0;
}

int csync_walker(CSYNC *ctx, std::unique_ptr<csync_file_stat_t> fs) {
  int rc = -1;

  if (ctx->abort) {
    qCDebug(lcUpdate, "Aborted!");
    ctx->status_code = CSYNC_STATUS_ABORTED;
    return -1;
  }

  switch (fs->type) {
    case ItemTypeFile:
      if (ctx->current == REMOTE_REPLICA) {
          qCDebug(lcUpdate, "file: %s [file_id=%s size=%" PRIu64 "]", fs->path.constData(), fs->file_id.constData(), fs->size);
      } else {
          qCDebug(lcUpdate, "file: %s [inode=%" PRIu64 " size=%" PRIu64 "]", fs->path.constData(), fs->inode, fs->size);
      }
      break;
  case ItemTypeDirectory: /* enter directory */
      if (ctx->current == REMOTE_REPLICA) {
          qCDebug(lcUpdate, "directory: %s [file_id=%s]", fs->path.constData(), fs->file_id.constData());
      } else {
          qCDebug(lcUpdate, "directory: %s [inode=%" PRIu64 "]", fs->path.constData(), fs->inode);
      }
      break;
  case ItemTypeSoftLink:
    qCInfo(lcUpdate, "symlink: %s - not supported", fs->path.constData());
    break;
  default:
    qCInfo(lcUpdate, "item: %s - item type %d not iterated", fs->path.constData(), fs->type);
    return 0;
  }

  rc = _csync_detect_update(ctx, std::move(fs));

  return rc;
}

static bool fill_tree_from_db(CSYNC *ctx, const char *uri)
{
    int64_t count = 0;
    QByteArray skipbase;
    auto &files = ctx->current == LOCAL_REPLICA ? ctx->local.files : ctx->remote.files;
    auto rowCallback = [ctx, &count, &skipbase, &files](const OCC::SyncJournalFileRecord &rec) {
        if (ctx->current == REMOTE_REPLICA) {
            /* When selective sync is used, the database may have subtrees with a parent
             * whose etag is _invalid_. These are ignored and shall not appear in the
             * remote tree.
             * Sometimes folders that are not ignored by selective sync get marked as
             * _invalid_, but that is not a problem as the next discovery will retrieve
             * their correct etags again and we don't run into this case.
             */
            if (rec._etag == "_invalid_") {
                qCInfo(lcUpdate, "%s selective sync excluded", rec._path.constData());
                skipbase = rec._path;
                skipbase += '/';
                return;
            }

            /* Skip over all entries with the same base path. Note that this depends
             * strongly on the ordering of the retrieved items. */
            if (!skipbase.isEmpty() && rec._path.startsWith(skipbase)) {
                qCDebug(lcUpdate, "%s selective sync excluded because the parent is", rec._path.constData());
                return;
            } else {
                skipbase.clear();
            }
        }

        std::unique_ptr<csync_file_stat_t> st = csync_file_stat_t::fromSyncJournalFileRecord(rec);

        /* Check for exclusion from the tree.
         * Note that this is only a safety net in case the ignore list changes
         * without a full remote discovery being triggered. */
        CSYNC_EXCLUDE_TYPE excluded = CSYNC_NOT_EXCLUDED;
        if (ctx->exclude_traversal_fn)
            excluded = ctx->exclude_traversal_fn(st->path, st->type);
        if (excluded != CSYNC_NOT_EXCLUDED) {
            qInfo(lcUpdate, "%s excluded from db read (%d)", st->path.constData(), excluded);

            if (excluded == CSYNC_FILE_EXCLUDE_AND_REMOVE
                    || excluded == CSYNC_FILE_SILENTLY_EXCLUDED) {
                return;
            }

            st->instruction = CSYNC_INSTRUCTION_IGNORE;
        }

        /* store into result list. */
        files[rec._path] = std::move(st);
        ++count;
    };

    if (!ctx->statedb->getFilesBelowPath(uri, rowCallback)) {
        ctx->status_code = CSYNC_STATUS_STATEDB_LOAD_ERROR;
        return false;
    }
    qInfo(lcUpdate, "%" PRId64 " entries read below path %s from db.", count, uri);

    return true;
}

/* set the current item to an ignored state.
 * If the item is set to ignored, the update phase continues, ie. its not a hard error */
static bool mark_current_item_ignored( CSYNC *ctx, csync_file_stat_t *previous_fs, CSYNC_STATUS status )
{
    if(!ctx) {
        return false;
    }

    if (ctx->current_fs) {
        ctx->current_fs->instruction = CSYNC_INSTRUCTION_IGNORE;
        ctx->current_fs->error_status = status;
        /* If a directory has ignored files, put the flag on the parent directory as well */
        if( previous_fs ) {
            previous_fs->has_ignored_files = true;
        }
        return true;
    }
    return false;
}

/* File tree walker */
int csync_ftw(CSYNC *ctx, const char *uri, csync_walker_fn fn,
    unsigned int depth) {
  QByteArray filename;
  QByteArray fullpath;
  csync_vio_handle_t *dh = NULL;
  std::unique_ptr<csync_file_stat_t> dirent;
  csync_file_stat_t *previous_fs = NULL;
  int read_from_db = 0;
  int rc = 0;

  bool do_read_from_db = (ctx->current == REMOTE_REPLICA && ctx->remote.read_from_db);
  const char *db_uri = uri;

  if (ctx->current == LOCAL_REPLICA && ctx->should_discover_locally_fn) {
      const char *local_uri = uri + strlen(ctx->local.uri);
      if (*local_uri == '/')
          ++local_uri;
      db_uri = local_uri;
      do_read_from_db = !ctx->should_discover_locally_fn(QByteArray(local_uri));
  }

  if (!depth) {
    mark_current_item_ignored(ctx, previous_fs, CSYNC_STATUS_INDIVIDUAL_TOO_DEEP);
    return 0;
  }

  read_from_db = ctx->remote.read_from_db;

  // if the etag of this dir is still the same, its content is restored from the
  // database.
  if( do_read_from_db ) {
      if( ! fill_tree_from_db(ctx, db_uri) ) {
        errno = ENOENT;
        ctx->status_code = CSYNC_STATUS_OPENDIR_ERROR;
        goto error;
      }
      return 0;
  }

  if ((dh = csync_vio_opendir(ctx, uri)) == NULL) {
      if (ctx->abort) {
          qCDebug(lcUpdate, "Aborted!");
          ctx->status_code = CSYNC_STATUS_ABORTED;
          goto error;
      }
      int asp = 0;
      /* permission denied */
      ctx->status_code = csync_errno_to_status(errno, CSYNC_STATUS_OPENDIR_ERROR);
      if (errno == EACCES) {
          qCWarning(lcUpdate, "Permission denied.");
          if (mark_current_item_ignored(ctx, previous_fs, CSYNC_STATUS_PERMISSION_DENIED)) {
              return 0;
          }
      } else if(errno == ENOENT) {
          asp = asprintf( &ctx->error_string, "%s", uri);
          ASSERT(asp >= 0);
      }
      // 403 Forbidden can be sent by the server if the file firewall is active.
      // A file or directory should be ignored and sync must continue. See #3490
      else if(errno == ERRNO_FORBIDDEN) {
          qCWarning(lcUpdate, "Directory access Forbidden (File Firewall?)");
          if( mark_current_item_ignored(ctx, previous_fs, CSYNC_STATUS_FORBIDDEN) ) {
              return 0;
          }
          /* if current_fs is not defined here, better throw an error */
      }
      // The server usually replies with the custom "503 Storage not available"
      // if some path is temporarily unavailable. But in some cases a standard 503
      // is returned too. Thus we can't distinguish the two and will treat any
      // 503 as request to ignore the folder. See #3113 #2884.
      else if(errno == ERRNO_STORAGE_UNAVAILABLE || errno == ERRNO_SERVICE_UNAVAILABLE) {
          qCWarning(lcUpdate, "Storage was not available!");
          if( mark_current_item_ignored(ctx, previous_fs, CSYNC_STATUS_STORAGE_UNAVAILABLE ) ) {
              return 0;
          }
          /* if current_fs is not defined here, better throw an error */
      } else {
          qCWarning(lcUpdate, "opendir failed for %s - errno %d", uri, errno);
      }
      goto error;
  }

  while (true) {
    // Get the next item in the directory
    errno = 0;
    dirent = csync_vio_readdir(ctx, dh);
    if (!dirent) {
        if (errno != 0) {
            // Note: Windows vio converts any error into EACCES
            qCWarning(lcUpdate, "readdir failed for file in %s - errno %d", uri, errno);
            goto error;
        }

        // Normal case: End of items in directory
        break;
    }

    /* Conversion error */
    if (dirent->path.isEmpty() && !dirent->original_path.isEmpty()) {
        ctx->status_code = CSYNC_STATUS_INVALID_CHARACTERS;
        ctx->error_string = c_strdup(dirent->original_path);
        dirent->original_path.clear();
        goto error;
    }

    // At this point dirent->path only contains the file name.
    filename = dirent->path;
    if (filename.isEmpty()) {
      ctx->status_code = CSYNC_STATUS_READDIR_ERROR;
      goto error;
    }

    /* skip "." and ".." */
    if ( filename == "." || filename == "..") {
      continue;
    }

    if (uri[0] == '\0') {
        fullpath = filename;
    } else {
        fullpath = QByteArray() % uri % '/' % filename;
    }

    /* if the filename starts with a . we consider it a hidden file
     * For windows, the hidden state is also discovered within the vio
     * local stat function.
     */
    if( filename[0] == '.' ) {
        if (filename != ".sys.admin#recall#") { /* recall file shall not be ignored (#4420) */
            dirent->is_hidden = true;
        }
    }

    // Now process to have a relative path to the sync root for the local replica, or to the data root on the remote.
    dirent->path = fullpath;
    if (ctx->current == LOCAL_REPLICA) {
        ASSERT(dirent->path.startsWith(ctx->local.uri)); // path is relative to uri
        // "len + 1" to include the slash in-between.
        size_t uriLength = strlen(ctx->local.uri);
        dirent->path = dirent->path.mid(OCC::Utility::convertSizeToInt(uriLength) + 1);
    }

    previous_fs = ctx->current_fs;
    bool recurse = dirent->type == ItemTypeDirectory;

    /* Call walker function for each file */
    rc = fn(ctx, std::move(dirent));
    /* this function may update ctx->current and ctx->read_from_db */

    if (rc < 0) {
      if (CSYNC_STATUS_IS_OK(ctx->status_code)) {
          ctx->status_code = CSYNC_STATUS_UPDATE_ERROR;
      }

      ctx->current_fs = previous_fs;
      goto error;
    }

    if (recurse && rc == 0
        && (!ctx->current_fs || ctx->current_fs->instruction != CSYNC_INSTRUCTION_IGNORE)) {
      rc = csync_ftw(ctx, fullpath, fn, depth - 1);
      if (rc < 0) {
        ctx->current_fs = previous_fs;
        goto error;
      }

      if (ctx->current_fs && !ctx->current_fs->child_modified
          && ctx->current_fs->instruction == CSYNC_INSTRUCTION_EVAL) {
          if (ctx->current == REMOTE_REPLICA) {
              ctx->current_fs->instruction = CSYNC_INSTRUCTION_UPDATE_METADATA;
          } else {
              ctx->current_fs->instruction = CSYNC_INSTRUCTION_NONE;
          }
      }

      if (ctx->current_fs && previous_fs && ctx->current_fs->has_ignored_files) {
          /* If a directory has ignored files, put the flag on the parent directory as well */
          previous_fs->has_ignored_files = ctx->current_fs->has_ignored_files;
      }
    }

    if (ctx->current_fs && previous_fs && ctx->current_fs->child_modified) {
        /* If a directory has modified files, put the flag on the parent directory as well */
        previous_fs->child_modified = ctx->current_fs->child_modified;
    }

    ctx->current_fs = previous_fs;
    ctx->remote.read_from_db = read_from_db;
  }

  csync_vio_closedir(ctx, dh);
  qCInfo(lcUpdate, " <= Closing walk for %s with read_from_db %d", uri, read_from_db);

  return rc;

error:
  ctx->remote.read_from_db = read_from_db;
  if (dh != NULL) {
    csync_vio_closedir(ctx, dh);
  }
  return -1;
}

/* vim: set ts=8 sw=2 et cindent: */
