/*
 * libcsync -- a library to sync a directory with another
 *
 * Copyright (c) 2008-2013 by Andreas Schneider <asn@cryptomilk.org>
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

#include <cassert>
#include "csync_private.h"
#include "csync_reconcile.h"
#include "csync_util.h"
#include "csync_rename.h"
#include "common/c_jhash.h"
#include "common/asserts.h"
#include "common/syncjournalfilerecord.h"

#include <QFileInfo>
#include <QLoggingCategory>
Q_LOGGING_CATEGORY(lcReconcile, "nextcloud.sync.csync.reconciler", QtInfoMsg)

// Needed for PRIu64 on MinGW in C++ mode.
#define __STDC_FORMAT_MACROS
#include <cinttypes>

/* Check if a file is ignored because one parent is ignored.
 * return the node of the ignored directoy if it's the case, or \c nullptr if it is not ignored */
static csync_file_stat_t *_csync_check_ignored(csync_s::FileMap *tree, const ByteArrayRef &path)
{
    /* compute the size of the parent directory */
    int parentlen = path.size() - 1;
    while (parentlen > 0 && path.at(parentlen) != '/') {
        parentlen--;
    }
    if (parentlen <= 0) {
        return nullptr;
    }
    ByteArrayRef parentPath = path.left(parentlen);
    csync_file_stat_t *fs = tree->findFile(parentPath);
    if (fs) {
        if (fs->instruction == CSYNC_INSTRUCTION_IGNORE) {
            /* Yes, we are ignored */
            return fs;
        } else {
            /* Not ignored */
            return nullptr;
        }
    } else {
        /* Try if the parent itself is ignored */
        return _csync_check_ignored(tree, parentPath);
    }
}


/**
 * The main function in the reconcile pass.
 *
 * It's called for each entry in the local and remote files by
 * csync_reconcile()
 *
 * Before the reconcile phase the trees already know about changes
 * relative to the sync journal. This function's job is to spot conflicts
 * between local and remote changes and adjust the nodes accordingly.
 *
 * See doc/dev/sync-algorithm.md for an overview.
 *
 *
 * Older detail comment:
 *
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
static void _csync_merge_algorithm_visitor(csync_file_stat_t *cur, CSYNC * ctx) {
    csync_s::FileMap *our_tree = nullptr;
    csync_s::FileMap *other_tree = nullptr;

    /* we need the opposite tree! */
    switch (ctx->current) {
    case LOCAL_REPLICA:
        our_tree = &ctx->local.files;
        other_tree = &ctx->remote.files;
        break;
    case REMOTE_REPLICA:
        our_tree = &ctx->remote.files;
        other_tree = &ctx->local.files;
        break;
    default:
        break;
    }

    csync_file_stat_t *other = other_tree->findFile(cur->path);
    if (!other) {
        if (ctx->current == REMOTE_REPLICA) {
            // The file was not found and the other tree is the local one
            // check if the path doesn't match a mangled file name
            other = other_tree->findFileMangledName(cur->path);
        } else {
            other = other_tree->findFile(cur->e2eMangledName);
        }
    }

    if (!other) {
        /* Check the renamed path as well. */
        other = other_tree->findFile(csync_rename_adjust_parent_path(ctx, cur->path));
    }
    if (!other) {
        /* Check if it is ignored */
        other = _csync_check_ignored(other_tree, cur->path);
        /* If it is ignored, other->instruction will be  IGNORE so this one will also be ignored */
    }

    /* file only found on current replica */
    if (!other) {
        switch(cur->instruction) {
        /* file has been modified */
        case CSYNC_INSTRUCTION_EVAL:
            cur->instruction = CSYNC_INSTRUCTION_NEW;
            break;
            /* file has been removed on the opposite replica */
        case CSYNC_INSTRUCTION_NONE:
        case CSYNC_INSTRUCTION_UPDATE_METADATA:
            if (cur->has_ignored_files) {
                /* Do not remove a directory that has ignored files */
                break;
            }
            if (cur->child_modified) {
                /* re-create directory that has modified contents */
                cur->instruction = CSYNC_INSTRUCTION_NEW;
                break;
            }

            if (ctx->current == REMOTE_REPLICA) {
                auto localFileExists = QFileInfo{QString{ctx->local.uri} + "/" + cur->path};
                if (localFileExists.exists()) {
                    cur->instruction = CSYNC_INSTRUCTION_NONE;
                    qCWarning(lcReconcile()) << "preventing delete of remote file when local one was missing but not really" << cur->path;
                    break;
                }
            }

            cur->instruction = CSYNC_INSTRUCTION_REMOVE;
            break;
        case CSYNC_INSTRUCTION_EVAL_RENAME: {
            // By default, the EVAL_RENAME decays into a NEW
            cur->instruction = CSYNC_INSTRUCTION_NEW;

            bool processedRename = false;
            auto renameCandidateProcessing = [&](const QByteArray &basePath) {
                if (processedRename)
                    return;
                if (basePath.isEmpty())
                    return;

                /* First, check that the file is NOT in our tree (another file with the same name was added) */
                if (our_tree->findFile(basePath)) {
                    other = nullptr;
                    qCInfo(lcReconcile, "Origin found in our tree : %s", basePath.constData());
                } else {
                    /* Find the potential rename source file in the other tree.
                    * If the renamed file could not be found in the opposite tree, that is because it
                    * is not longer existing there, maybe because it was renamed or deleted.
                    * The journal is cleaned up later after propagation.
                    */
                    other = other_tree->findFile(basePath);
                    qCInfo(lcReconcile, "Rename origin in other tree (%s) %s",
                        basePath.constData(), other ? "found" : "not found");
                }

                const auto curParentPath = [=]{
                    const auto slashPosition = cur->path.lastIndexOf('/');
                    if (slashPosition >= 0) {
                        return cur->path.left(slashPosition);
                    } else {
                        return QByteArray();
                    }
                }();
                auto curParent = our_tree->findFile(curParentPath);

                if (!other
                 || !other->e2eMangledName.isEmpty()
                 || (curParent && curParent->isE2eEncrypted)) {
                    // Stick with the NEW since there's no "other" file
                    // or if there's an "other" file it involves E2EE and
                    // we want to always issue delete + upload in such cases
                    return;
                } else if (other->instruction == CSYNC_INSTRUCTION_RENAME) {
                    // Some other EVAL_RENAME already claimed other.
                    // We do nothing: maybe a different candidate for
                    // other is found as well?
                    qCInfo(lcReconcile, "Other has already been renamed to %s",
                        other->rename_path.constData());
                } else if (cur->type == ItemTypeDirectory
                    // The local replica is reconciled first, so the remote tree would
                    // have either NONE or UPDATE_METADATA if the remote file is safe to
                    // move.
                    // In the remote replica, REMOVE is also valid (local has already
                    // been reconciled). NONE can still happen if the whole parent dir
                    // was set to REMOVE by the local reconcile.
                    || other->instruction == CSYNC_INSTRUCTION_NONE
                    || other->instruction == CSYNC_INSTRUCTION_UPDATE_METADATA
                    || other->instruction == CSYNC_INSTRUCTION_REMOVE) {
                    qCInfo(lcReconcile, "Switching %s to RENAME to %s",
                        other->path.constData(), cur->path.constData());
                    other->instruction = CSYNC_INSTRUCTION_RENAME;
                    other->rename_path = cur->path;
                    if( !cur->file_id.isEmpty() ) {
                        other->file_id = cur->file_id;
                    }
                    if (ctx->current == LOCAL_REPLICA) {
                        // Keep the local mtime.
                        other->modtime = cur->modtime;
                    }
                    other->inode = cur->inode;
                    cur->instruction = CSYNC_INSTRUCTION_NONE;
                    // We have consumed 'other': exit this loop to not consume another one.
                    processedRename = true;
                } else if (our_tree->findFile(csync_rename_adjust_parent_path(ctx, other->path)) == cur) {
                    // If we're here, that means that the other side's reconcile will be able
                    // to work against cur: The filename itself didn't change, only a parent
                    // directory was renamed! In that case it's safe to ignore the rename
                    // since the parent directory rename will already deal with it.

                    // Local: The remote reconcile will be able to deal with this.
                    // Remote: The local replica has already dealt with this.
                    //         See the EVAL_RENAME case when other was found directly.
                    qCInfo(lcReconcile, "File in a renamed directory, other side's instruction: %d",
                        other->instruction);
                    cur->instruction = CSYNC_INSTRUCTION_NONE;
                } else {
                    // This can, for instance, happen when there was a local change in other
                    // and the instruction in the local tree is NEW while cur has EVAL_RENAME
                    // due to a remote move of the same file. In these scenarios we just
                    // want the instruction to stay NEW.
                    qCInfo(lcReconcile, "Other already has instruction %d",
                        other->instruction);
                }
            };

            if (ctx->current == LOCAL_REPLICA) {
                /* use the old name to find the "other" node */
                OCC::SyncJournalFileRecord base;
                qCInfo(lcReconcile, "Finding rename origin through inode %" PRIu64 "",
                    cur->inode);
                ctx->statedb->getFileRecordByInode(cur->inode, &base);
                renameCandidateProcessing(base._path);
            } else {
                ASSERT(ctx->current == REMOTE_REPLICA);

                // The update phase has already mapped out all dir->dir renames, check the
                // path that is consistent with that first. Otherwise update mappings and
                // reconcile mappings might disagree, leading to odd situations down the
                // line.
                auto basePath = csync_rename_adjust_full_path_source(ctx, cur->path);
                if (basePath != cur->path) {
                    qCInfo(lcReconcile, "Trying rename origin by csync_rename mapping %s",
                        basePath.constData());
                    // We go through getFileRecordsByFileId to ensure the basePath
                    // computed in this way also has the expected fileid.
                    ctx->statedb->getFileRecordsByFileId(cur->file_id,
                        [&](const OCC::SyncJournalFileRecord &base) {
                            if (base._path == basePath)
                                renameCandidateProcessing(basePath);
                        });
                }

                // Also feed all the other files with the same fileid if necessary
                if (!processedRename) {
                    qCInfo(lcReconcile, "Finding rename origin through file ID %s",
                        cur->file_id.constData());
                    ctx->statedb->getFileRecordsByFileId(cur->file_id,
                        [&](const OCC::SyncJournalFileRecord &base) { renameCandidateProcessing(base._path); });
                }
            }

            break;
        }
        default:
            break;
        }
    } else {
        bool is_conflict = true;
        /*
     * file found on the other replica
     */

        switch (cur->instruction) {
        case CSYNC_INSTRUCTION_UPDATE_METADATA:
            if (other->instruction == CSYNC_INSTRUCTION_UPDATE_METADATA && ctx->current == LOCAL_REPLICA) {
                // Remote wins, the SyncEngine will pick relevant local metadata since the remote tree is walked last.
                cur->instruction = CSYNC_INSTRUCTION_NONE;
            }
            break;
        case CSYNC_INSTRUCTION_EVAL_RENAME:
            /* If the file already exist on the other side, we have a conflict.
               Abort the rename and consider it is a new file. */
            cur->instruction = CSYNC_INSTRUCTION_NEW;
            /* fall through */
        /* file on current replica is changed or new */
        case CSYNC_INSTRUCTION_EVAL:
        case CSYNC_INSTRUCTION_NEW:
            switch (other->instruction) {
            /* file on other replica is changed or new */
            case CSYNC_INSTRUCTION_NEW:
            case CSYNC_INSTRUCTION_EVAL:
                if (other->type == ItemTypeDirectory &&
                        cur->type == ItemTypeDirectory) {
                    // Folders of the same path are always considered equals
                    is_conflict = false;
                } else {
                    // If the size or mtime is different, it's definitely a conflict.
                    is_conflict = ((other->size != cur->size) || (other->modtime != cur->modtime));

                    // It could be a conflict even if size and mtime match!
                    //
                    // In older client versions we always treated these cases as a
                    // non-conflict. This behavior is preserved in case the server
                    // doesn't provide a content checksum.
                    //
                    // When it does have one, however, we do create a job, but the job
                    // will compare hashes and avoid the download if possible.
                    QByteArray remoteChecksumHeader =
                        (ctx->current == REMOTE_REPLICA ? cur->checksumHeader : other->checksumHeader);
                    if (!remoteChecksumHeader.isEmpty()) {
                        is_conflict = true;

                        // Do we have an UploadInfo for this?
                        // Maybe the Upload was completed, but the connection was broken just before
                        // we recieved the etag (Issue #5106)
                        auto up = ctx->statedb->getUploadInfo(cur->path);
                        if (up._valid && up._contentChecksum == remoteChecksumHeader) {
                            // Solve the conflict into an upload, or nothing
                            auto remoteNode = ctx->current == REMOTE_REPLICA ? cur : other;
                            auto localNode = ctx->current == REMOTE_REPLICA ? other : cur;
                            remoteNode->instruction = CSYNC_INSTRUCTION_NONE;
                            localNode->instruction = up._modtime == localNode->modtime && up._size == localNode->size ?
                                CSYNC_INSTRUCTION_UPDATE_METADATA : CSYNC_INSTRUCTION_SYNC;

                            // Update the etag and other server metadata in the journal already
                            // (We can't use a typical CSYNC_INSTRUCTION_UPDATE_METADATA because
                            // we must not store the size/modtime from the file system)
                            OCC::SyncJournalFileRecord rec;
                            if (ctx->statedb->getFileRecord(remoteNode->path, &rec)) {
                                rec._path = remoteNode->path;
                                rec._etag = remoteNode->etag;
                                rec._fileId = remoteNode->file_id;
                                rec._modtime = remoteNode->modtime;
                                rec._type = remoteNode->type;
                                rec._fileSize = remoteNode->size;
                                rec._remotePerm = remoteNode->remotePerm;
                                rec._checksumHeader = remoteNode->checksumHeader;
                                ctx->statedb->setFileRecordMetadata(rec);
                            }
                            break;
                        }
                    }

                    // SO: If there is no checksum, we can have !is_conflict here
                    // even though the files have different content! This is an
                    // intentional tradeoff. Downloading and comparing files would
                    // be technically correct in this situation but leads to too
                    // much waste.
                    // In particular this kind of NEW/NEW situation with identical
                    // sizes and mtimes pops up when the local database is lost for
                    // whatever reason.
                }
                if (ctx->current == REMOTE_REPLICA) {
                    // If the files are considered equal, only update the DB with the etag from remote
                    cur->instruction = is_conflict ? CSYNC_INSTRUCTION_CONFLICT : CSYNC_INSTRUCTION_UPDATE_METADATA;
                    other->instruction = CSYNC_INSTRUCTION_NONE;
                } else {
                    cur->instruction = CSYNC_INSTRUCTION_NONE;
                    other->instruction = is_conflict ? CSYNC_INSTRUCTION_CONFLICT : CSYNC_INSTRUCTION_UPDATE_METADATA;
                }

                break;
                /* file on the other replica has not been modified */
            case CSYNC_INSTRUCTION_NONE:
            case CSYNC_INSTRUCTION_UPDATE_METADATA:
                if (cur->type != other->type) {
                    // If the type of the entity changed, it's like NEW, but
                    // needs to delete the other entity first.
                    cur->instruction = CSYNC_INSTRUCTION_TYPE_CHANGE;
                    other->instruction = CSYNC_INSTRUCTION_NONE;
                } else if (cur->type == ItemTypeDirectory) {
                    cur->instruction = CSYNC_INSTRUCTION_UPDATE_METADATA;
                    other->instruction = CSYNC_INSTRUCTION_NONE;
                } else {
                    cur->instruction = CSYNC_INSTRUCTION_SYNC;
                    other->instruction = CSYNC_INSTRUCTION_NONE;
                }
                break;
            case CSYNC_INSTRUCTION_IGNORE:
                cur->instruction = CSYNC_INSTRUCTION_IGNORE;
                break;
            default:
                break;
            }
            // Ensure we're not leaving discovery-only instructions
            // in place. This can happen, for instance, when other's
            // instruction is EVAL_RENAME because the parent dir was renamed.
            // NEW is safer than EVAL because it will end up with
            // propagation unless it's changed by something, and EVAL and
            // NEW are treated equivalently during reconcile.
            if (cur->instruction == CSYNC_INSTRUCTION_EVAL)
                cur->instruction = CSYNC_INSTRUCTION_NEW;
            break;
        default:
            break;
        }
    }

    //hide instruction NONE messages when log level is set to debug,
    //only show these messages on log level trace
    const char *repo = ctx->current == REMOTE_REPLICA ? "server" : "client";
    if(cur->instruction ==CSYNC_INSTRUCTION_NONE)
    {
        if(cur->type == ItemTypeDirectory)
        {
            qCDebug(lcReconcile,
                      "%-30s %s dir:  %s",
                      csync_instruction_str(cur->instruction),
                      repo,
                      cur->path.constData());
        }
        else
        {
            qCDebug(lcReconcile,
                      "%-30s %s file: %s",
                      csync_instruction_str(cur->instruction),
                      repo,
                      cur->path.constData());
        }
    }
    else
    {
        if(cur->type == ItemTypeDirectory)
        {
            qCInfo(lcReconcile,
                      "%-30s %s dir:  %s",
                      csync_instruction_str(cur->instruction),
                      repo,
                      cur->path.constData());
        }
        else
        {
            qCInfo(lcReconcile,
                      "%-30s %s file: %s",
                      csync_instruction_str(cur->instruction),
                      repo,
                      cur->path.constData());
        }
    }
}

void csync_reconcile_updates(CSYNC *ctx) {
  csync_s::FileMap *tree = nullptr;

  switch (ctx->current) {
    case LOCAL_REPLICA:
      tree = &ctx->local.files;
      break;
    case REMOTE_REPLICA:
      tree = &ctx->remote.files;
      break;
    default:
      break;
  }

  for (auto &pair : *tree) {
    _csync_merge_algorithm_visitor(pair.second.get(), ctx);
  }
}

/* vim: set ts=8 sw=2 et cindent: */
