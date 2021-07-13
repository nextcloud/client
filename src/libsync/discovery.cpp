/*
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "discovery.h"
#include "common/filesystembase.h"
#include "common/syncjournaldb.h"
#include "syncfileitem.h"
#include <QDebug>
#include <algorithm>
#include <set>
#include <QTextCodec>
#include "vio/csync_vio_local.h"
#include <QFileInfo>
#include <QFile>
#include <QThreadPool>
#include <common/checksums.h>
#include <common/constants.h>
#include "csync_exclude.h"
#include "csync.h"


namespace OCC {

Q_LOGGING_CATEGORY(lcDisco, "sync.discovery", QtInfoMsg)

void ProcessDirectoryJob::start()
{
    qCInfo(lcDisco) << "STARTING" << _currentFolder._server << _queryServer << _currentFolder._local << _queryLocal;

    if (_queryServer == NormalQuery) {
        _serverJob = startAsyncServerQuery();
    } else {
        _serverQueryDone = true;
    }

    // Check whether a normal local query is even necessary
    if (_queryLocal == NormalQuery) {
        if (!_discoveryData->_shouldDiscoverLocaly(_currentFolder._local)
            && (_currentFolder._local == _currentFolder._original || !_discoveryData->_shouldDiscoverLocaly(_currentFolder._original))) {
            _queryLocal = ParentNotChanged;
        }
    }

    if (_queryLocal == NormalQuery) {
        startAsyncLocalQuery();
    } else {
        _localQueryDone = true;
    }

    if (_localQueryDone && _serverQueryDone) {
        process();
    }
}

void ProcessDirectoryJob::process()
{
    ASSERT(_localQueryDone && _serverQueryDone);

    QString localDir;

    // Build lookup tables for local, remote and db entries.
    // For suffix-virtual files, the key will normally be the base file name
    // without the suffix.
    // However, if foo and foo.owncloud exists locally, there'll be "foo"
    // with local, db, server entries and "foo.owncloud" with only a local
    // entry.
    struct Entries {
        QString nameOverride;
        SyncJournalFileRecord dbEntry;
        RemoteInfo serverEntry;
        LocalInfo localEntry;
    };
    std::map<QString, Entries> entries;
    for (auto &e : _serverNormalQueryEntries) {
        entries[e.name].serverEntry = std::move(e);
    }
    _serverNormalQueryEntries.clear();

    // fetch all the name from the DB
    auto pathU8 = _currentFolder._original.toUtf8();
    if (!_discoveryData->_statedb->listFilesInPath(pathU8, [&](const SyncJournalFileRecord &rec) {
            auto name = pathU8.isEmpty() ? rec._path : QString::fromUtf8(rec._path.constData() + (pathU8.size() + 1));
            if (rec.isVirtualFile() && isVfsWithSuffix())
                chopVirtualFileSuffix(name);
            auto &dbEntry = entries[name].dbEntry;
            dbEntry = rec;
            setupDbPinStateActions(dbEntry);
        })) {
        dbError();
        return;
    }

    for (auto &e : _localNormalQueryEntries) {
        entries[e.name].localEntry = e;
    }
    if (isVfsWithSuffix()) {
        // For vfs-suffix the local data for suffixed files should usually be associated
        // with the non-suffixed name. Unless both names exist locally or there's
        // other data about the suffixed file.
        // This is done in a second path in order to not depend on the order of
        // _localNormalQueryEntries.
        for (auto &e : _localNormalQueryEntries) {
            if (!e.isVirtualFile)
                continue;
            auto &suffixedEntry = entries[e.name];
            bool hasOtherData = suffixedEntry.serverEntry.isValid() || suffixedEntry.dbEntry.isValid();

            auto nonvirtualName = e.name;
            chopVirtualFileSuffix(nonvirtualName);
            auto &nonvirtualEntry = entries[nonvirtualName];
            // If the non-suffixed entry has no data, move it
            if (!nonvirtualEntry.localEntry.isValid()) {
                std::swap(nonvirtualEntry.localEntry, suffixedEntry.localEntry);
                if (!hasOtherData)
                    entries.erase(e.name);
            } else if (!hasOtherData) {
                // Normally a lone local suffixed file would be processed under the
                // unsuffixed name. In this special case it's under the suffixed name.
                // To avoid lots of special casing, make sure PathTuple::addName()
                // will be called with the unsuffixed name anyway.
                suffixedEntry.nameOverride = nonvirtualName;
            }
        }
    }
    _localNormalQueryEntries.clear();

    //
    // Iterate over entries and process them
    //
    for (const auto &f : entries) {
        const auto &e = f.second;

        PathTuple path;
        path = _currentFolder.addName(e.nameOverride.isEmpty() ? f.first : e.nameOverride);

        if (isVfsWithSuffix()) {
            // Without suffix vfs the paths would be good. But since the dbEntry and localEntry
            // can have different names from f.first when suffix vfs is on, make sure the
            // corresponding _original and _local paths are right.

            if (e.dbEntry.isValid()) {
                path._original = e.dbEntry._path;
            } else if (e.localEntry.isVirtualFile) {
                // We don't have a db entry - but it should be at this path
                path._original = PathTuple::pathAppend(_currentFolder._original,  e.localEntry.name);
            }
            if (e.localEntry.isValid()) {
                path._local = PathTuple::pathAppend(_currentFolder._local, e.localEntry.name);
            } else if (e.dbEntry.isVirtualFile()) {
                // We don't have a local entry - but it should be at this path
                addVirtualFileSuffix(path._local);
            }
        }

        // On the server the path is mangled in case of E2EE
        if (!e.serverEntry.e2eMangledName.isEmpty()) {
            Q_ASSERT(_discoveryData->_remoteFolder.startsWith('/'));
            Q_ASSERT(_discoveryData->_remoteFolder.endsWith('/'));

            const auto rootPath = _discoveryData->_remoteFolder.mid(1);
            Q_ASSERT(e.serverEntry.e2eMangledName.startsWith(rootPath));

            path._server = e.serverEntry.e2eMangledName.mid(rootPath.length());
        }

        // If the filename starts with a . we consider it a hidden file
        // For windows, the hidden state is also discovered within the vio
        // local stat function.
        // Recall file shall not be ignored (#4420)
        bool isHidden = e.localEntry.isHidden || (!f.first.isEmpty() && f.first[0] == '.' && f.first != QLatin1String(".sys.admin#recall#"));
#ifdef Q_OS_WIN
        // exclude ".lnk" files as they are not essential, but, causing troubles when enabling the VFS due to QFileInfo::isDir() and other methods are freezing, which causes the ".lnk" files to start hydrating and freezing the app eventually.
        const bool isServerEntryWindowsShortcut = !e.localEntry.isValid() && e.serverEntry.isValid() && !e.serverEntry.isDirectory && FileSystem::isLnkFile(e.serverEntry.name);
#else
        const bool isServerEntryWindowsShortcut = false;
#endif
        if (handleExcluded(path._target, e.localEntry.name,
                e.localEntry.isDirectory || e.serverEntry.isDirectory, isHidden,
                e.localEntry.isSymLink || isServerEntryWindowsShortcut))
            continue;

        if (_queryServer == InBlackList || _discoveryData->isInSelectiveSyncBlackList(path._original)) {
            processBlacklisted(path, e.localEntry, e.dbEntry);
            continue;
        }
        processFile(std::move(path), e.localEntry, e.serverEntry, e.dbEntry);
    }
    QTimer::singleShot(0, _discoveryData, &DiscoveryPhase::scheduleMoreJobs);
}

bool ProcessDirectoryJob::handleExcluded(const QString &path, const QString &localName, bool isDirectory, bool isHidden, bool isSymlink)
{
    auto excluded = _discoveryData->_excludes->traversalPatternMatch(path, isDirectory ? ItemTypeDirectory : ItemTypeFile);

    // FIXME: move to ExcludedFiles 's regexp ?
    bool isInvalidPattern = false;
    if (excluded == CSYNC_NOT_EXCLUDED && !_discoveryData->_invalidFilenameRx.isEmpty()) {
        if (path.contains(_discoveryData->_invalidFilenameRx)) {
            excluded = CSYNC_FILE_EXCLUDE_INVALID_CHAR;
            isInvalidPattern = true;
        }
    }
    if (excluded == CSYNC_NOT_EXCLUDED && _discoveryData->_ignoreHiddenFiles && isHidden) {
        excluded = CSYNC_FILE_EXCLUDE_HIDDEN;
    }
    if (excluded == CSYNC_NOT_EXCLUDED && !localName.isEmpty()
            && _discoveryData->_serverBlacklistedFiles.contains(localName)) {
        excluded = CSYNC_FILE_EXCLUDE_SERVER_BLACKLISTED;
        isInvalidPattern = true;
    }

    auto localCodec = QTextCodec::codecForLocale();
    if (localCodec->mibEnum() != 106) {
        // If the locale codec is not UTF-8, we must check that the filename from the server can
        // be encoded in the local file system.
        //
        // We cannot use QTextCodec::canEncode() since that can incorrectly return true, see
        // https://bugreports.qt.io/browse/QTBUG-6925.
        QTextEncoder encoder(localCodec, QTextCodec::ConvertInvalidToNull);
        if (encoder.fromUnicode(path).contains('\0')) {
            qCWarning(lcDisco) << "Cannot encode " << path << " to local encoding " << localCodec->name();
            excluded = CSYNC_FILE_EXCLUDE_CANNOT_ENCODE;
        }
    }

    if (excluded == CSYNC_NOT_EXCLUDED && !isSymlink) {
        return false;
    } else if (excluded == CSYNC_FILE_SILENTLY_EXCLUDED || excluded == CSYNC_FILE_EXCLUDE_AND_REMOVE) {
        emit _discoveryData->silentlyExcluded(path);
        return true;
    }

    auto item = SyncFileItemPtr::create();
    item->_file = path;
    item->_originalFile = path;
    item->_instruction = CSYNC_INSTRUCTION_IGNORE;

    if (isSymlink) {
        /* Symbolic links are ignored. */
        item->_errorString = tr("Symbolic links are not supported in syncing.");
    } else {
        switch (excluded) {
        case CSYNC_NOT_EXCLUDED:
        case CSYNC_FILE_SILENTLY_EXCLUDED:
        case CSYNC_FILE_EXCLUDE_AND_REMOVE:
            qFatal("These were handled earlier");
        case CSYNC_FILE_EXCLUDE_LIST:
            item->_errorString = tr("File is listed on the ignore list.");
            break;
        case CSYNC_FILE_EXCLUDE_INVALID_CHAR:
            if (item->_file.endsWith('.')) {
                item->_errorString = tr("File names ending with a period are not supported on this file system.");
            } else {
                char invalid = '\0';
                foreach (char x, QByteArray("\\:?*\"<>|")) {
                    if (item->_file.contains(x)) {
                        invalid = x;
                        break;
                    }
                }
                if (invalid) {
                    item->_errorString = tr("File names containing the character '%1' are not supported on this file system.")
                                             .arg(QLatin1Char(invalid));
                }
                if (isInvalidPattern) {
                    item->_errorString = tr("File name contains at least one invalid character");
                } else {
                    item->_errorString = tr("The file name is a reserved name on this file system.");
                }
            }
            item->_status = SyncFileItem::FileNameInvalid;
            break;
        case CSYNC_FILE_EXCLUDE_TRAILING_SPACE:
            item->_errorString = tr("Filename contains trailing spaces.");
            item->_status = SyncFileItem::FileNameInvalid;
            break;
        case CSYNC_FILE_EXCLUDE_LONG_FILENAME:
            item->_errorString = tr("Filename is too long.");
            item->_status = SyncFileItem::FileNameInvalid;
            break;
        case CSYNC_FILE_EXCLUDE_HIDDEN:
            item->_errorString = tr("File/Folder is ignored because it's hidden.");
            break;
        case CSYNC_FILE_EXCLUDE_STAT_FAILED:
            item->_errorString = tr("Stat failed.");
            break;
        case CSYNC_FILE_EXCLUDE_CONFLICT:
            item->_errorString = tr("Conflict: Server version downloaded, local copy renamed and not uploaded.");
            item->_status = SyncFileItem::Conflict;
        break;
        case CSYNC_FILE_EXCLUDE_CANNOT_ENCODE:
            item->_errorString = tr("The filename cannot be encoded on your file system.");
            break;
        case CSYNC_FILE_EXCLUDE_SERVER_BLACKLISTED:
            item->_errorString = tr("The filename is blacklisted on the server.");
            break;
        }
    }

    _childIgnored = true;
    emit _discoveryData->itemDiscovered(item);
    return true;
}

void ProcessDirectoryJob::processFile(PathTuple path,
    const LocalInfo &localEntry, const RemoteInfo &serverEntry,
    const SyncJournalFileRecord &dbEntry)
{
    const char *hasServer = serverEntry.isValid() ? "true" : _queryServer == ParentNotChanged ? "db" : "false";
    const char *hasLocal = localEntry.isValid() ? "true" : _queryLocal == ParentNotChanged ? "db" : "false";
    qCInfo(lcDisco).nospace() << "Processing " << path._original
                              << " | valid: " << dbEntry.isValid() << "/" << hasLocal << "/" << hasServer
                              << " | mtime: " << dbEntry._modtime << "/" << localEntry.modtime << "/" << serverEntry.modtime
                              << " | size: " << dbEntry._fileSize << "/" << localEntry.size << "/" << serverEntry.size
                              << " | etag: " << dbEntry._etag << "//" << serverEntry.etag
                              << " | checksum: " << dbEntry._checksumHeader << "//" << serverEntry.checksumHeader
                              << " | perm: " << dbEntry._remotePerm << "//" << serverEntry.remotePerm
                              << " | fileid: " << dbEntry._fileId << "//" << serverEntry.fileId
                              << " | inode: " << dbEntry._inode << "/" << localEntry.inode << "/"
                              << " | type: " << dbEntry._type << "/" << localEntry.type << "/" << (serverEntry.isDirectory ? ItemTypeDirectory : ItemTypeFile)
                              << " | e2ee: " << dbEntry._isE2eEncrypted << "/" << serverEntry.isE2eEncrypted
                              << " | e2eeMangledName: " << dbEntry.e2eMangledName() << "/" << serverEntry.e2eMangledName;

    if (localEntry.isValid()
        && !serverEntry.isValid()
        && !dbEntry.isValid()
        && localEntry.modtime < _lastSyncTimestamp) {
        qCWarning(lcDisco) << "File" << path._original << "was modified before the last sync run and is not in the sync journal and server";
    }

    if (_discoveryData->isRenamed(path._original)) {
        qCDebug(lcDisco) << "Ignoring renamed";
        return; // Ignore this.
    }

    auto item = SyncFileItem::fromSyncJournalFileRecord(dbEntry);
    item->_file = path._target;
    item->_originalFile = path._original;
    item->_previousSize = dbEntry._fileSize;
    item->_previousModtime = dbEntry._modtime;

    if (dbEntry._modtime == localEntry.modtime && dbEntry._type == ItemTypeVirtualFile && localEntry.type == ItemTypeFile) {
        item->_type = ItemTypeFile;
    }

    // The item shall only have this type if the db request for the virtual download
    // was successful (like: no conflicting remote remove etc). This decision is done
    // either in processFileAnalyzeRemoteInfo() or further down here.
    if (item->_type == ItemTypeVirtualFileDownload)
        item->_type = ItemTypeVirtualFile;
    // Similarly db entries with a dehydration request denote a regular file
    // until the request is processed.
    if (item->_type == ItemTypeVirtualFileDehydration)
        item->_type = ItemTypeFile;

    // VFS suffixed files on the server are ignored
    if (isVfsWithSuffix()) {
        if (hasVirtualFileSuffix(serverEntry.name)
            || (localEntry.isVirtualFile && !dbEntry.isVirtualFile() && hasVirtualFileSuffix(dbEntry._path))) {
            item->_instruction = CSYNC_INSTRUCTION_IGNORE;
            item->_errorString = tr("File has extension reserved for virtual files.");
            _childIgnored = true;
            emit _discoveryData->itemDiscovered(item);
            return;
        }
    }

    if (serverEntry.isValid()) {
        processFileAnalyzeRemoteInfo(item, path, localEntry, serverEntry, dbEntry);
        return;
    }

    // Downloading a virtual file is like a server action and can happen even if
    // server-side nothing has changed
    // NOTE: Normally setting the VirtualFileDownload flag means that local and
    // remote will be rediscovered. This is just a fallback for a similar check
    // in processFileAnalyzeRemoteInfo().
    if (_queryServer == ParentNotChanged
        && dbEntry.isValid()
        && (dbEntry._type == ItemTypeVirtualFileDownload
            || localEntry.type == ItemTypeVirtualFileDownload)
        && (localEntry.isValid() || _queryLocal == ParentNotChanged)) {
        item->_direction = SyncFileItem::Down;
        item->_instruction = CSYNC_INSTRUCTION_SYNC;
        item->_type = ItemTypeVirtualFileDownload;
    }

    processFileAnalyzeLocalInfo(item, path, localEntry, serverEntry, dbEntry, _queryServer);
}

// Compute the checksum of the given file and assign the result in item->_checksumHeader
// Returns true if the checksum was successfully computed
static bool computeLocalChecksum(const QByteArray &header, const QString &path, const SyncFileItemPtr &item)
{
    auto type = parseChecksumHeaderType(header);
    if (!type.isEmpty()) {
        // TODO: compute async?
        QByteArray checksum = ComputeChecksum::computeNowOnFile(path, type);
        if (!checksum.isEmpty()) {
            item->_checksumHeader = makeChecksumHeader(type, checksum);
            return true;
        }
    }
    return false;
}

void ProcessDirectoryJob::processFileAnalyzeRemoteInfo(
    const SyncFileItemPtr &item, PathTuple path, const LocalInfo &localEntry,
    const RemoteInfo &serverEntry, const SyncJournalFileRecord &dbEntry)
{
    item->_checksumHeader = serverEntry.checksumHeader;
    item->_fileId = serverEntry.fileId;
    item->_remotePerm = serverEntry.remotePerm;
    item->_type = serverEntry.isDirectory ? ItemTypeDirectory : ItemTypeFile;
    item->_etag = serverEntry.etag;
    item->_directDownloadUrl = serverEntry.directDownloadUrl;
    item->_directDownloadCookies = serverEntry.directDownloadCookies;
    item->_isEncrypted = serverEntry.isE2eEncrypted;
    item->_encryptedFileName = [=] {
        if (serverEntry.e2eMangledName.isEmpty()) {
            return QString();
        }

        Q_ASSERT(_discoveryData->_remoteFolder.startsWith('/'));
        Q_ASSERT(_discoveryData->_remoteFolder.endsWith('/'));

        const auto rootPath = _discoveryData->_remoteFolder.mid(1);
        Q_ASSERT(serverEntry.e2eMangledName.startsWith(rootPath));
        return serverEntry.e2eMangledName.mid(rootPath.length());
    }();

    // Check for missing server data
    {
        QStringList missingData;
        if (serverEntry.size == -1)
            missingData.append(tr("size"));
        if (serverEntry.remotePerm.isNull())
            missingData.append(tr("permissions"));
        if (serverEntry.etag.isEmpty())
            missingData.append("ETag");
        if (serverEntry.fileId.isEmpty())
            missingData.append(tr("file id"));
        if (!missingData.isEmpty()) {
            item->_instruction = CSYNC_INSTRUCTION_ERROR;
            _childIgnored = true;
            item->_errorString = tr("server reported no %1").arg(missingData.join(QLatin1String(", ")));
            emit _discoveryData->itemDiscovered(item);
            return;
        }
    }

    // The file is known in the db already
    if (dbEntry.isValid()) {
        qint64 size = serverEntry.size;

        bool metaDataSizeNeedsUpdateForE2EEFile = false;

        if (dbEntry.isVirtualFile() && (!item->_encryptedFileName.isEmpty()) && size > 0) {
            // make sure we set correct size when file was downloaded previously and has now been changed on the server
            // serverEntry always includes extra CommonConstants::e2EeTagSize bytes for e2e encrypted files
            // we don't need those neither when creating a placeholder nor when storing hydrated file on disk
            size = serverEntry.size - CommonConstants::e2EeTagSize;
            metaDataSizeNeedsUpdateForE2EEFile = dbEntry._fileSize == serverEntry.size;
        }

        if (serverEntry.isDirectory != dbEntry.isDirectory()) {
            // If the type of the entity changed, it's like NEW, but
            // needs to delete the other entity first.
            item->_instruction = CSYNC_INSTRUCTION_TYPE_CHANGE;
            item->_direction = SyncFileItem::Down;
            item->_modtime = serverEntry.modtime;
            item->_size = size;
        } else if ((dbEntry._type == ItemTypeVirtualFileDownload || localEntry.type == ItemTypeVirtualFileDownload)
            && (localEntry.isValid() || _queryLocal == ParentNotChanged)) {
            // The above check for the localEntry existing is important. Otherwise it breaks
            // the case where a file is moved and simultaneously tagged for download in the db.
            item->_direction = SyncFileItem::Down;
            item->_instruction = CSYNC_INSTRUCTION_SYNC;
            item->_type = ItemTypeVirtualFileDownload;
        } else if (dbEntry._etag != serverEntry.etag) {
            item->_direction = SyncFileItem::Down;
            item->_modtime = serverEntry.modtime;
            item->_size = size;
            if (serverEntry.isDirectory) {
                ENFORCE(dbEntry.isDirectory());
                item->_instruction = CSYNC_INSTRUCTION_UPDATE_METADATA;
            } else if (!localEntry.isValid() && _queryLocal != ParentNotChanged) {
                // Deleted locally, changed on server
                item->_instruction = CSYNC_INSTRUCTION_NEW;
            } else {
                item->_instruction = CSYNC_INSTRUCTION_SYNC;
            }
        } else if (dbEntry._remotePerm != serverEntry.remotePerm || dbEntry._fileId != serverEntry.fileId || metaDataSizeNeedsUpdateForE2EEFile) {
            if (metaDataSizeNeedsUpdateForE2EEFile) {
                item->_size = serverEntry.size - CommonConstants::e2EeTagSize;
            }
            item->_instruction = CSYNC_INSTRUCTION_UPDATE_METADATA;
            item->_direction = SyncFileItem::Down;
        } else {
            // if (is virtual mode enabled and folder is encrypted - check if the size is the same as on the server and then - trigger server query
            // to update a placeholder with corrected size (-16 Bytes)
            // or, maybe, add a flag to the database - vfsE2eeSizeCorrected? if it is not set - subtract it from the placeholder's size and re-create/update a placeholder?
            QueryMode serverQueryMode = ParentNotChanged;
            if (dbEntry.isDirectory() && dbEntry._isE2eEncrypted) {
                qint64 totalSizeOfPath = 0;
                if (_discoveryData->_statedb->listFilesInPath(dbEntry.path().toUtf8(), [this, &totalSizeOfPath](const OCC::SyncJournalFileRecord &record) {
                        if (record.isFile()) {
                            totalSizeOfPath += record._fileSize + CommonConstants::e2EeTagSize;
                        } else if (record.isVirtualFile()) {
                            totalSizeOfPath += record._fileSize;
                        }
                    })) {
                    if (totalSizeOfPath != 0 && totalSizeOfPath == serverEntry.sizeOfFolder) {
                        serverQueryMode = NormalQuery;
                    }
                }
            }
            processFileAnalyzeLocalInfo(item, path, localEntry, serverEntry, dbEntry, serverQueryMode);
            return;
        }

        processFileAnalyzeLocalInfo(item, path, localEntry, serverEntry, dbEntry, _queryServer);
        return;
    }

    // Unknown in db: new file on the server
    Q_ASSERT(!dbEntry.isValid());

    item->_instruction = CSYNC_INSTRUCTION_NEW;
    item->_direction = SyncFileItem::Down;
    item->_modtime = serverEntry.modtime;
    item->_size = serverEntry.size;

    auto postProcessServerNew = [=] () {
        auto tmp_path = path;
        if (item->isDirectory()) {
            _pendingAsyncJobs++;
            _discoveryData->checkSelectiveSyncNewFolder(tmp_path._server, serverEntry.remotePerm,
                [=](bool result) {
                    --_pendingAsyncJobs;
                    if (!result) {
                        processFileAnalyzeLocalInfo(item, tmp_path, localEntry, serverEntry, dbEntry, _queryServer);
                    }
                    QTimer::singleShot(0, _discoveryData, &DiscoveryPhase::scheduleMoreJobs);
                });
            return;
        }
        // Turn new remote files into virtual files if the option is enabled.
        auto &opts = _discoveryData->_syncOptions;
        if (!localEntry.isValid()
            && item->_type == ItemTypeFile
            && opts._vfs->mode() != Vfs::Off
            && _pinState != PinState::AlwaysLocal
            && !FileSystem::isExcludeFile(item->_file)) {
            item->_type = ItemTypeVirtualFile;
            if (!item->_encryptedFileName.isEmpty()) {
                // We are syncing a file for the first time (local entry is invalid) and it is encrypted file that will be virtual once synced
                // to avoid having error of "file has changed during sync" when trying to hydrate it excplicitly - we must remove CommonConstants::e2EeTagSize bytes from the end
                // as explicit hydration does not care if these bytes are present in the placeholder or not, but, the size must not change in the middle of the sync
                // this way it works for both implicit and explicit hydration by making a placeholder size that does not includes encryption tag CommonConstants::e2EeTagSize bytes
                item->_size = serverEntry.size - CommonConstants::e2EeTagSize;
            }
            if (isVfsWithSuffix())
                addVirtualFileSuffix(tmp_path._original);
        }
        processFileAnalyzeLocalInfo(item, tmp_path, localEntry, serverEntry, dbEntry, _queryServer);
    };

    // Potential NEW/NEW conflict is handled in AnalyzeLocal
    if (localEntry.isValid()) {
        postProcessServerNew();
        return;
    }

    // Not in db or locally: either new or a rename
    Q_ASSERT(!dbEntry.isValid() && !localEntry.isValid());

    // Check for renames (if there is a file with the same file id)
    bool done = false;
    bool async = false;
    // This function will be executed for every candidate
    auto renameCandidateProcessing = [&](const OCC::SyncJournalFileRecord &base) {
        if (done)
            return;
        if (!base.isValid())
            return;

        // Remote rename of a virtual file we have locally scheduled for download.
        if (base._type == ItemTypeVirtualFileDownload) {
            // We just consider this NEW but mark it for download.
            item->_type = ItemTypeVirtualFileDownload;
            done = true;
            return;
        }

        // Remote rename targets a file that shall be locally dehydrated.
        if (base._type == ItemTypeVirtualFileDehydration) {
            // Don't worry about the rename, just consider it DELETE + NEW(virtual)
            done = true;
            return;
        }

        // Some things prohibit rename detection entirely.
        // Since we don't do the same checks again in reconcile, we can't
        // just skip the candidate, but have to give up completely.
        if (base.isDirectory() != item->isDirectory()) {
            qCInfo(lcDisco, "file types different, not a rename");
            done = true;
            return;
        }
        if (!serverEntry.isDirectory && base._etag != serverEntry.etag) {
            /* File with different etag, don't do a rename, but download the file again */
            qCInfo(lcDisco, "file etag different, not a rename");
            done = true;
            return;
        }

        // Now we know there is a sane rename candidate.
        QString originalPath = base.path();

        if (_discoveryData->isRenamed(originalPath)) {
            qCInfo(lcDisco, "folder already has a rename entry, skipping");
            return;
        }

        /* A remote rename can also mean Encryption Mangled Name.
         * if we find one of those in the database, we ignore it.
         */
        if (!base._e2eMangledName.isEmpty()) {
            qCWarning(lcDisco, "Encrypted file can not rename");
            done = true;
            return;
        }

        QString originalPathAdjusted = _discoveryData->adjustRenamedPath(originalPath, SyncFileItem::Up);

        if (!base.isDirectory()) {
            csync_file_stat_t buf;
            if (csync_vio_local_stat(_discoveryData->_localDir + originalPathAdjusted, &buf)) {
                qCInfo(lcDisco) << "Local file does not exist anymore." << originalPathAdjusted;
                return;
            }
            // NOTE: This prohibits some VFS renames from being detected since
            // suffix-file size is different from the db size. That's ok, they'll DELETE+NEW.
            if (buf.modtime != base._modtime || buf.size != base._fileSize || buf.type == ItemTypeDirectory) {
                qCInfo(lcDisco) << "File has changed locally, not a rename." << originalPath;
                return;
            }
        } else {
            if (!QFileInfo(_discoveryData->_localDir + originalPathAdjusted).isDir()) {
                qCInfo(lcDisco) << "Local directory does not exist anymore." << originalPathAdjusted;
                return;
            }
        }

        // Renames of virtuals are possible
        if (base.isVirtualFile()) {
            item->_type = ItemTypeVirtualFile;
        }

        bool wasDeletedOnServer = _discoveryData->findAndCancelDeletedJob(originalPath).first;

        auto postProcessRename = [this, item, base, originalPath](PathTuple &path) {
            auto adjustedOriginalPath = _discoveryData->adjustRenamedPath(originalPath, SyncFileItem::Up);
            _discoveryData->_renamedItemsRemote.insert(originalPath, path._target);
            item->_modtime = base._modtime;
            item->_inode = base._inode;
            item->_instruction = CSYNC_INSTRUCTION_RENAME;
            item->_direction = SyncFileItem::Down;
            item->_renameTarget = path._target;
            item->_file = adjustedOriginalPath;
            item->_originalFile = originalPath;
            path._original = originalPath;
            path._local = adjustedOriginalPath;
            qCInfo(lcDisco) << "Rename detected (down) " << item->_file << " -> " << item->_renameTarget;
        };

        if (wasDeletedOnServer) {
            postProcessRename(path);
            done = true;
        } else {
            // we need to make a request to the server to know that the original file is deleted on the server
            _pendingAsyncJobs++;
            auto job = new RequestEtagJob(_discoveryData->_account, originalPath, this);
            connect(job, &RequestEtagJob::finishedWithResult, this, [=](const HttpResult<QString> &etag) {
                auto tmp_path = path;
                _pendingAsyncJobs--;
                QTimer::singleShot(0, _discoveryData, &DiscoveryPhase::scheduleMoreJobs);
                if (etag || etag.error().code != 404 ||
                    // Somehow another item claimed this original path, consider as if it existed
                    _discoveryData->isRenamed(originalPath)) {
                    // If the file exist or if there is another error, consider it is a new file.
                    postProcessServerNew();
                    return;
                }

                // The file do not exist, it is a rename

                // In case the deleted item was discovered in parallel
                _discoveryData->findAndCancelDeletedJob(originalPath);

                postProcessRename(tmp_path);
                processFileFinalize(item, tmp_path, item->isDirectory(), item->_instruction == CSYNC_INSTRUCTION_RENAME ? NormalQuery : ParentDontExist, _queryServer);
            });
            job->start();
            done = true; // Ideally, if the origin still exist on the server, we should continue searching...  but that'd be difficult
            async = true;
        }
    };
    if (!_discoveryData->_statedb->getFileRecordsByFileId(serverEntry.fileId, renameCandidateProcessing)) {
        dbError();
        return;
    }
    if (async) {
        return; // We went async
    }

    if (item->_instruction == CSYNC_INSTRUCTION_NEW) {
        postProcessServerNew();
        return;
    }
    processFileAnalyzeLocalInfo(item, path, localEntry, serverEntry, dbEntry, _queryServer);
}

void ProcessDirectoryJob::processFileAnalyzeLocalInfo(
    const SyncFileItemPtr &item, PathTuple path, const LocalInfo &localEntry,
    const RemoteInfo &serverEntry, const SyncJournalFileRecord &dbEntry, QueryMode recurseQueryServer)
{
    bool noServerEntry = (_queryServer != ParentNotChanged && !serverEntry.isValid())
        || (_queryServer == ParentNotChanged && !dbEntry.isValid());

    if (noServerEntry)
        recurseQueryServer = ParentDontExist;

    bool serverModified = item->_instruction == CSYNC_INSTRUCTION_NEW || item->_instruction == CSYNC_INSTRUCTION_SYNC
        || item->_instruction == CSYNC_INSTRUCTION_RENAME || item->_instruction == CSYNC_INSTRUCTION_TYPE_CHANGE;

    // Decay server modifications to UPDATE_METADATA if the local virtual exists
    bool hasLocalVirtual = localEntry.isVirtualFile || (_queryLocal == ParentNotChanged && dbEntry.isVirtualFile());
    bool virtualFileDownload = item->_type == ItemTypeVirtualFileDownload;
    if (serverModified && !virtualFileDownload && hasLocalVirtual) {
        item->_instruction = CSYNC_INSTRUCTION_UPDATE_METADATA;
        serverModified = false;
        item->_type = ItemTypeVirtualFile;
    }

    if (dbEntry.isVirtualFile() && (!localEntry.isValid() || localEntry.isVirtualFile) && !virtualFileDownload) {
        item->_type = ItemTypeVirtualFile;
    }

    _childModified |= serverModified;

    auto finalize = [&] {
        bool recurse = item->isDirectory() || localEntry.isDirectory || serverEntry.isDirectory;
        // Even if we have a local directory: If the remote is a file that's propagated as a
        // conflict we don't need to recurse into it. (local c1.owncloud, c1/ ; remote: c1)
        if (item->_instruction == CSYNC_INSTRUCTION_CONFLICT && !item->isDirectory())
            recurse = false;
        if (_queryLocal != NormalQuery && _queryServer != NormalQuery)
            recurse = false;

        auto recurseQueryLocal = _queryLocal == ParentNotChanged ? ParentNotChanged : localEntry.isDirectory || item->_instruction == CSYNC_INSTRUCTION_RENAME ? NormalQuery : ParentDontExist;
        processFileFinalize(item, path, recurse, recurseQueryLocal, recurseQueryServer);
    };

    if (!localEntry.isValid()) {
        if (_queryLocal == ParentNotChanged && dbEntry.isValid()) {
            // Not modified locally (ParentNotChanged)
            if (noServerEntry) {
                // not on the server: Removed on the server, delete locally
                qCInfo(lcDisco) << "File" << item->_file << "is not anymore on server. Going to delete it locally.";
                item->_instruction = CSYNC_INSTRUCTION_REMOVE;
                item->_direction = SyncFileItem::Down;
            } else if (dbEntry._type == ItemTypeVirtualFileDehydration) {
                // dehydration requested
                item->_direction = SyncFileItem::Down;
                item->_instruction = CSYNC_INSTRUCTION_SYNC;
                item->_type = ItemTypeVirtualFileDehydration;
            }
        } else if (noServerEntry) {
            // Not locally, not on the server. The entry is stale!
            qCInfo(lcDisco) << "Stale DB entry";
            _discoveryData->_statedb->deleteFileRecord(path._original, true);
            return;
        } else if (dbEntry._type == ItemTypeVirtualFile && isVfsWithSuffix()) {
            // If the virtual file is removed, recreate it.
            // This is a precaution since the suffix files don't look like the real ones
            // and we don't want users to accidentally delete server data because they
            // might not expect that deleting the placeholder will have a remote effect.
            item->_instruction = CSYNC_INSTRUCTION_NEW;
            item->_direction = SyncFileItem::Down;
            item->_type = ItemTypeVirtualFile;
        } else if (!serverModified) {
            // Removed locally: also remove on the server.
            if (!dbEntry._serverHasIgnoredFiles) {
                qCInfo(lcDisco) << "File" << item->_file << "was deleted locally. Going to delete it on the server.";
                item->_instruction = CSYNC_INSTRUCTION_REMOVE;
                item->_direction = SyncFileItem::Up;
            }
        }

        finalize();
        return;
    }

    Q_ASSERT(localEntry.isValid());

    item->_inode = localEntry.inode;

    if (dbEntry.isValid()) {
        bool typeChange = localEntry.isDirectory != dbEntry.isDirectory();
        if (!typeChange && localEntry.isVirtualFile) {
            if (noServerEntry) {
                item->_instruction = CSYNC_INSTRUCTION_REMOVE;
                item->_direction = SyncFileItem::Down;
            } else if (!dbEntry.isVirtualFile() && isVfsWithSuffix()) {
                // If we find what looks to be a spurious "abc.owncloud" the base file "abc"
                // might have been renamed to that. Make sure that the base file is not
                // deleted from the server.
                if (dbEntry._modtime == localEntry.modtime && dbEntry._fileSize == localEntry.size) {
                    qCInfo(lcDisco) << "Base file was renamed to virtual file:" << item->_file;
                    item->_direction = SyncFileItem::Down;
                    item->_instruction = CSYNC_INSTRUCTION_SYNC;
                    item->_type = ItemTypeVirtualFileDehydration;
                    addVirtualFileSuffix(item->_file);
                    item->_renameTarget = item->_file;
                } else {
                    qCInfo(lcDisco) << "Virtual file with non-virtual db entry, ignoring:" << item->_file;
                    item->_instruction = CSYNC_INSTRUCTION_IGNORE;
                }
            }
        } else if (!typeChange && ((dbEntry._modtime == localEntry.modtime && dbEntry._fileSize == localEntry.size) || localEntry.isDirectory)) {
            // Local file unchanged.
            if (noServerEntry) {
                qCInfo(lcDisco) << "File" << item->_file << "is not anymore on server. Going to delete it locally.";
                item->_instruction = CSYNC_INSTRUCTION_REMOVE;
                item->_direction = SyncFileItem::Down;
            } else if (dbEntry._type == ItemTypeVirtualFileDehydration || localEntry.type == ItemTypeVirtualFileDehydration) {
                item->_direction = SyncFileItem::Down;
                item->_instruction = CSYNC_INSTRUCTION_SYNC;
                item->_type = ItemTypeVirtualFileDehydration;
            } else if (!serverModified
                && (dbEntry._inode != localEntry.inode
                    || _discoveryData->_syncOptions._vfs->needsMetadataUpdate(*item))) {
                item->_instruction = CSYNC_INSTRUCTION_UPDATE_METADATA;
                item->_direction = SyncFileItem::Down;
            }
        } else if (!typeChange && isVfsWithSuffix()
            && dbEntry.isVirtualFile() && !localEntry.isVirtualFile
            && dbEntry._inode == localEntry.inode
            && dbEntry._modtime == localEntry.modtime
            && localEntry.size == 1) {
            // A suffix vfs file can be downloaded by renaming it to remove the suffix.
            // This check leaks some details of VfsSuffix, particularly the size of placeholders.
            item->_direction = SyncFileItem::Down;
            if (noServerEntry) {
                item->_instruction = CSYNC_INSTRUCTION_REMOVE;
                item->_type = ItemTypeFile;
            } else {
                item->_instruction = CSYNC_INSTRUCTION_SYNC;
                item->_type = ItemTypeVirtualFileDownload;
                item->_previousSize = 1;
            }
        } else if (serverModified
            || (isVfsWithSuffix() && dbEntry.isVirtualFile())) {
            // There's a local change and a server change: Conflict!
            // Alternatively, this might be a suffix-file that's virtual in the db but
            // not locally. These also become conflicts. For in-place placeholders that's
            // not necessary: they could be replaced by real files and should then trigger
            // a regular SYNC upwards when there's no server change.
            processFileConflict(item, path, localEntry, serverEntry, dbEntry);
        } else if (typeChange) {
            item->_instruction = CSYNC_INSTRUCTION_TYPE_CHANGE;
            item->_direction = SyncFileItem::Up;
            item->_checksumHeader.clear();
            item->_size = localEntry.size;
            item->_modtime = localEntry.modtime;
            item->_type = localEntry.isDirectory ? ItemTypeDirectory : ItemTypeFile;
            _childModified = true;
        } else {
            // Local file was changed
            item->_instruction = CSYNC_INSTRUCTION_SYNC;
            if (noServerEntry) {
                // Special case! deleted on server, modified on client, the instruction is then NEW
                item->_instruction = CSYNC_INSTRUCTION_NEW;
            }
            item->_direction = SyncFileItem::Up;
            item->_checksumHeader.clear();
            item->_size = localEntry.size;
            item->_modtime = localEntry.modtime;
            _childModified = true;

            // Checksum comparison at this stage is only enabled for .eml files,
            // check #4754 #4755
            bool isEmlFile = path._original.endsWith(QLatin1String(".eml"), Qt::CaseInsensitive);
            if (isEmlFile && dbEntry._fileSize == localEntry.size && !dbEntry._checksumHeader.isEmpty()) {
                if (computeLocalChecksum(dbEntry._checksumHeader, _discoveryData->_localDir + path._local, item)
                        && item->_checksumHeader == dbEntry._checksumHeader) {
                    qCInfo(lcDisco) << "NOTE: Checksums are identical, file did not actually change: " << path._local;
                    item->_instruction = CSYNC_INSTRUCTION_UPDATE_METADATA;
                }
            }
        }

        finalize();
        return;
    }

    Q_ASSERT(!dbEntry.isValid());

    if (localEntry.isVirtualFile && !noServerEntry) {
        // Somehow there is a missing DB entry while the virtual file already exists.
        // The instruction should already be set correctly.
        ASSERT(item->_instruction == CSYNC_INSTRUCTION_UPDATE_METADATA);
        ASSERT(item->_type == ItemTypeVirtualFile);
        finalize();
        return;
    } else if (serverModified) {
        processFileConflict(item, path, localEntry, serverEntry, dbEntry);
        finalize();
        return;
    }

    // New local file or rename
    item->_instruction = CSYNC_INSTRUCTION_NEW;
    item->_direction = SyncFileItem::Up;
    item->_checksumHeader.clear();
    item->_size = localEntry.size;
    item->_modtime = localEntry.modtime;
    item->_type = localEntry.isDirectory ? ItemTypeDirectory : localEntry.isVirtualFile ? ItemTypeVirtualFile : ItemTypeFile;
    _childModified = true;

    auto postProcessLocalNew = [item, localEntry, path, this]() {
        // TODO: We may want to execute the same logic for non-VFS mode, as, moving/renaming the same folder by 2 or more clients at the same time is not possible in Web UI.
        // Keeping it like this (for VFS files and folders only) just to fix a user issue.

        if (!(_discoveryData && _discoveryData->_syncOptions._vfs && _discoveryData->_syncOptions._vfs->mode() != Vfs::Off)) {
            // for VFS files and folders only
            return;
        }

        if (!localEntry.isVirtualFile && !localEntry.isDirectory) {
            return;
        }

        Q_ASSERT(item->_instruction == CSYNC_INSTRUCTION_NEW);
        if (item->_instruction != CSYNC_INSTRUCTION_NEW) {
            qCWarning(lcDisco) << "Trying to wipe a virtual item" << path._local << " with item->_instruction" << item->_instruction;
            return;
        }

        // must be a dehydrated placeholder
        const bool isFilePlaceHolder = !localEntry.isDirectory && _discoveryData->_syncOptions._vfs->isDehydratedPlaceholder(_discoveryData->_localDir + path._local);

        // either correct availability, or a result with error if the folder is new or otherwise has no availability set yet
        const auto folderPlaceHolderAvailability = localEntry.isDirectory ? _discoveryData->_syncOptions._vfs->availability(path._local) : Vfs::AvailabilityResult(Vfs::AvailabilityError::NoSuchItem);

        const auto folderPinState = localEntry.isDirectory ? _discoveryData->_syncOptions._vfs->pinState(path._local) : Optional<PinStateEnums::PinState>(PinState::Unspecified);

        if (!isFilePlaceHolder && !folderPlaceHolderAvailability.isValid() && !folderPinState.isValid()) {
            // not a file placeholder and not a synced folder placeholder (new local folder)
            return;
        }

        const auto isFolderPinStateOnlineOnly = (folderPinState.isValid() && *folderPinState == PinState::OnlineOnly);

        const auto isfolderPlaceHolderAvailabilityOnlineOnly = (folderPlaceHolderAvailability.isValid() && *folderPlaceHolderAvailability == VfsItemAvailability::OnlineOnly);

        // a folder is considered online-only if: no files are hydrated, or, if it's an empty folder
        const auto isOnlineOnlyFolder = isfolderPlaceHolderAvailabilityOnlineOnly || !folderPlaceHolderAvailability && isFolderPinStateOnlineOnly;

        if (!isFilePlaceHolder && !isOnlineOnlyFolder) {
            if (localEntry.isDirectory && folderPlaceHolderAvailability.isValid() && !isOnlineOnlyFolder) {
                // a VFS folder but is not online0only (has some files hydrated)
                qCInfo(lcDisco) << "Virtual directory without db entry for" << path._local << "but it contains hydrated file(s), so let's keep it and reupload.";
                emit _discoveryData->addErrorToGui(SyncFileItem::SoftError, tr("Conflict when uploading some files to a folder. Those, conflicted, are going to get cleared!"), path._local);
                return;
            }
            qCWarning(lcDisco) << "Virtual file without db entry for" << path._local
                               << "but looks odd, keeping";
            item->_instruction = CSYNC_INSTRUCTION_IGNORE;

            return;
        }

        if (isOnlineOnlyFolder) {
            // if we're wiping a folder, we will only get this function called once and will wipe a folder along with it's files and also display one error in GUI
            qCInfo(lcDisco) << "Wiping virtual folder without db entry for" << path._local;
            emit _discoveryData->addErrorToGui(SyncFileItem::SoftError, tr("Conflict when uploading a folder. It's going to get cleared!"), path._local);
        } else {
            qCInfo(lcDisco) << "Wiping virtual file without db entry for" << path._local;
            emit _discoveryData->addErrorToGui(SyncFileItem::SoftError, tr("Conflict when uploading a file. It's going to get removed!"), path._local);
        }
        item->_instruction = CSYNC_INSTRUCTION_REMOVE;
        item->_direction = SyncFileItem::Down;
        // this flag needs to be unset, otherwise a folder would get marked as new in the processSubJobs
        _childModified = false;
    };

    // Check if it is a move
    OCC::SyncJournalFileRecord base;
    if (!_discoveryData->_statedb->getFileRecordByInode(localEntry.inode, &base)) {
        dbError();
        return;
    }
    const auto originalPath = base.path();

    // Function to gradually check conditions for accepting a move-candidate
    auto moveCheck = [&]() {
        if (!base.isValid()) {
            qCInfo(lcDisco) << "Not a move, no item in db with inode" << localEntry.inode;
            return false;
        }

        if (base._isE2eEncrypted || isInsideEncryptedTree()) {
            return false;
        }

        if (base.isDirectory() != item->isDirectory()) {
            qCInfo(lcDisco) << "Not a move, types don't match" << base._type << item->_type << localEntry.type;
            return false;
        }
        // Directories and virtual files don't need size/mtime equality
        if (!localEntry.isDirectory && !base.isVirtualFile()
            && (base._modtime != localEntry.modtime || base._fileSize != localEntry.size)) {
            qCInfo(lcDisco) << "Not a move, mtime or size differs, "
                            << "modtime:" << base._modtime << localEntry.modtime << ", "
                            << "size:" << base._fileSize << localEntry.size;
            return false;
        }

        // The old file must have been deleted.
        if (QFile::exists(_discoveryData->_localDir + originalPath)
            // Exception: If the rename changes case only (like "foo" -> "Foo") the
            // old filename might still point to the same file.
            && !(Utility::fsCasePreserving()
                 && originalPath.compare(path._local, Qt::CaseInsensitive) == 0
                 && originalPath != path._local)) {
            qCInfo(lcDisco) << "Not a move, base file still exists at" << originalPath;
            return false;
        }

        // Verify the checksum where possible
        if (!base._checksumHeader.isEmpty() && item->_type == ItemTypeFile && base._type == ItemTypeFile) {
            if (computeLocalChecksum(base._checksumHeader, _discoveryData->_localDir + path._original, item)) {
                qCInfo(lcDisco) << "checking checksum of potential rename " << path._original << item->_checksumHeader << base._checksumHeader;
                if (item->_checksumHeader != base._checksumHeader) {
                    qCInfo(lcDisco) << "Not a move, checksums differ";
                    return false;
                }
            }
        }

        if (_discoveryData->isRenamed(originalPath)) {
            qCInfo(lcDisco) << "Not a move, base path already renamed";
            return false;
        }

        return true;
    };

    // If it's not a move it's just a local-NEW
    if (!moveCheck()) {
        if (base._isE2eEncrypted) {
            // renaming the encrypted folder is done via remove + re-upload hence we need to mark the newly created folder as encrypted
            // base is a record in the SyncJournal database that contains the data about the being-renamed folder with it's old name and encryption information
            item->_isEncrypted = true;
        }
        postProcessLocalNew();
        finalize();
        return;
    }

    // Check local permission if we are allowed to put move the file here
    // Technically we should use the permissions from the server, but we'll assume it is the same
    auto movePerms = checkMovePermissions(base._remotePerm, originalPath, item->isDirectory());
    if (!movePerms.sourceOk || !movePerms.destinationOk) {
        qCInfo(lcDisco) << "Move without permission to rename base file, "
                        << "source:" << movePerms.sourceOk
                        << ", target:" << movePerms.destinationOk
                        << ", targetNew:" << movePerms.destinationNewOk;

        // If we can create the destination, do that.
        // Permission errors on the destination will be handled by checkPermissions later.
        postProcessLocalNew();
        finalize();

        // If the destination upload will work, we're fine with the source deletion.
        // If the source deletion can't work, checkPermissions will error.
        if (movePerms.destinationNewOk)
            return;

        // Here we know the new location can't be uploaded: must prevent the source delete.
        // Two cases: either the source item was already processed or not.
        auto wasDeletedOnClient = _discoveryData->findAndCancelDeletedJob(originalPath);
        if (wasDeletedOnClient.first) {
            // More complicated. The REMOVE is canceled. Restore will happen next sync.
            qCInfo(lcDisco) << "Undid remove instruction on source" << originalPath;
            _discoveryData->_statedb->deleteFileRecord(originalPath, true);
            _discoveryData->_statedb->schedulePathForRemoteDiscovery(originalPath);
            _discoveryData->_anotherSyncNeeded = true;
        } else {
            // Signal to future checkPermissions() to forbid the REMOVE and set to restore instead
            qCInfo(lcDisco) << "Preventing future remove on source" << originalPath;
            _discoveryData->_forbiddenDeletes[originalPath + '/'] = true;
        }
        return;
    }

    auto wasDeletedOnClient = _discoveryData->findAndCancelDeletedJob(originalPath);

    auto processRename = [item, originalPath, base, this](PathTuple &path) {
        auto adjustedOriginalPath = _discoveryData->adjustRenamedPath(originalPath, SyncFileItem::Down);
        _discoveryData->_renamedItemsLocal.insert(originalPath, path._target);
        item->_renameTarget = path._target;
        path._server = adjustedOriginalPath;
        item->_file = path._server;
        path._original = originalPath;
        item->_originalFile = path._original;
        item->_modtime = base._modtime;
        item->_inode = base._inode;
        item->_instruction = CSYNC_INSTRUCTION_RENAME;
        item->_direction = SyncFileItem::Up;
        item->_fileId = base._fileId;
        item->_remotePerm = base._remotePerm;
        item->_etag = base._etag;
        item->_type = base._type;

        // Discard any download/dehydrate tags on the base file.
        // They could be preserved and honored in a follow-up sync,
        // but it complicates handling a lot and will happen rarely.
        if (item->_type == ItemTypeVirtualFileDownload)
            item->_type = ItemTypeVirtualFile;
        if (item->_type == ItemTypeVirtualFileDehydration)
            item->_type = ItemTypeFile;

        qCInfo(lcDisco) << "Rename detected (up) " << item->_file << " -> " << item->_renameTarget;
    };
    if (wasDeletedOnClient.first) {
        recurseQueryServer = wasDeletedOnClient.second == base._etag ? ParentNotChanged : NormalQuery;
        processRename(path);
    } else {
        // We must query the server to know if the etag has not changed
        _pendingAsyncJobs++;
        QString serverOriginalPath = _discoveryData->_remoteFolder + _discoveryData->adjustRenamedPath(originalPath, SyncFileItem::Down);
        if (base.isVirtualFile() && isVfsWithSuffix())
            chopVirtualFileSuffix(serverOriginalPath);
        auto job = new RequestEtagJob(_discoveryData->_account, serverOriginalPath, this);
        connect(job, &RequestEtagJob::finishedWithResult, this, [=](const HttpResult<QString> &etag) mutable {
            if (!etag || (*etag != base._etag && !item->isDirectory()) || _discoveryData->isRenamed(originalPath)) {
                qCInfo(lcDisco) << "Can't rename because the etag has changed or the directory is gone" << originalPath;
                // Can't be a rename, leave it as a new.
                postProcessLocalNew();
            } else {
                // In case the deleted item was discovered in parallel
                _discoveryData->findAndCancelDeletedJob(originalPath);
                processRename(path);
                recurseQueryServer = *etag == base._etag ? ParentNotChanged : NormalQuery;
            }
            processFileFinalize(item, path, item->isDirectory(), NormalQuery, recurseQueryServer);
            _pendingAsyncJobs--;
            QTimer::singleShot(0, _discoveryData, &DiscoveryPhase::scheduleMoreJobs);
        });
        job->start();
        return;
    }

    finalize();
}

void ProcessDirectoryJob::processFileConflict(const SyncFileItemPtr &item, ProcessDirectoryJob::PathTuple path, const LocalInfo &localEntry, const RemoteInfo &serverEntry, const SyncJournalFileRecord &dbEntry)
{
    item->_previousSize = localEntry.size;
    item->_previousModtime = localEntry.modtime;

    if (serverEntry.isDirectory && localEntry.isDirectory) {
        // Folders of the same path are always considered equals
        item->_instruction = CSYNC_INSTRUCTION_UPDATE_METADATA;
        return;
    }

    // A conflict with a virtual should lead to virtual file download
    if (dbEntry.isVirtualFile() || localEntry.isVirtualFile)
        item->_type = ItemTypeVirtualFileDownload;

    // If there's no content hash, use heuristics
    if (serverEntry.checksumHeader.isEmpty()) {
        // If the size or mtime is different, it's definitely a conflict.
        bool isConflict = (serverEntry.size != localEntry.size) || (serverEntry.modtime != localEntry.modtime);

        // It could be a conflict even if size and mtime match!
        //
        // In older client versions we always treated these cases as a
        // non-conflict. This behavior is preserved in case the server
        // doesn't provide a content checksum.
        // SO: If there is no checksum, we can have !isConflict here
        // even though the files might have different content! This is an
        // intentional tradeoff. Downloading and comparing files would
        // be technically correct in this situation but leads to too
        // much waste.
        // In particular this kind of NEW/NEW situation with identical
        // sizes and mtimes pops up when the local database is lost for
        // whatever reason.
        item->_instruction = isConflict ? CSYNC_INSTRUCTION_CONFLICT : CSYNC_INSTRUCTION_UPDATE_METADATA;
        item->_direction = isConflict ? SyncFileItem::None : SyncFileItem::Down;
        return;
    }

    // Do we have an UploadInfo for this?
    // Maybe the Upload was completed, but the connection was broken just before
    // we recieved the etag (Issue #5106)
    auto up = _discoveryData->_statedb->getUploadInfo(path._original);
    if (up._valid && up._contentChecksum == serverEntry.checksumHeader) {
        // Solve the conflict into an upload, or nothing
        item->_instruction = up._modtime == localEntry.modtime && up._size == localEntry.size
            ? CSYNC_INSTRUCTION_NONE : CSYNC_INSTRUCTION_SYNC;
        item->_direction = SyncFileItem::Up;

        // Update the etag and other server metadata in the journal already
        // (We can't use a typical CSYNC_INSTRUCTION_UPDATE_METADATA because
        // we must not store the size/modtime from the file system)
        OCC::SyncJournalFileRecord rec;
        if (_discoveryData->_statedb->getFileRecord(path._original, &rec)) {
            rec._path = path._original.toUtf8();
            rec._etag = serverEntry.etag;
            rec._fileId = serverEntry.fileId;
            rec._modtime = serverEntry.modtime;
            rec._type = item->_type;
            rec._fileSize = serverEntry.size;
            rec._remotePerm = serverEntry.remotePerm;
            rec._checksumHeader = serverEntry.checksumHeader;
            _discoveryData->_statedb->setFileRecord(rec);
        }
        return;
    }

    // Rely on content hash comparisons to optimize away non-conflicts inside the job
    item->_instruction = CSYNC_INSTRUCTION_CONFLICT;
    item->_direction = SyncFileItem::None;
}

void ProcessDirectoryJob::processFileFinalize(
    const SyncFileItemPtr &item, PathTuple path, bool recurse,
    QueryMode recurseQueryLocal, QueryMode recurseQueryServer)
{
    // Adjust target path for virtual-suffix files
    if (isVfsWithSuffix()) {
        if (item->_type == ItemTypeVirtualFile) {
            addVirtualFileSuffix(path._target);
            if (item->_instruction == CSYNC_INSTRUCTION_RENAME)
                addVirtualFileSuffix(item->_renameTarget);
            else
                addVirtualFileSuffix(item->_file);
        }
        if (item->_type == ItemTypeVirtualFileDehydration
            && item->_instruction == CSYNC_INSTRUCTION_SYNC) {
            if (item->_renameTarget.isEmpty()) {
                item->_renameTarget = item->_file;
                addVirtualFileSuffix(item->_renameTarget);
            }
        }
    }

    if (path._original != path._target && (item->_instruction == CSYNC_INSTRUCTION_UPDATE_METADATA || item->_instruction == CSYNC_INSTRUCTION_NONE)) {
        ASSERT(_dirItem && _dirItem->_instruction == CSYNC_INSTRUCTION_RENAME);
        // This is because otherwise subitems are not updated!  (ideally renaming a directory could
        // update the database for all items!  See PropagateDirectory::slotSubJobsFinished)
        item->_instruction = CSYNC_INSTRUCTION_RENAME;
        item->_renameTarget = path._target;
        item->_direction = _dirItem->_direction;
    }

    qCInfo(lcDisco) << "Discovered" << item->_file << item->_instruction << item->_direction << item->_type;

    if (item->isDirectory() && item->_instruction == CSYNC_INSTRUCTION_SYNC)
        item->_instruction = CSYNC_INSTRUCTION_UPDATE_METADATA;
    bool removed = item->_instruction == CSYNC_INSTRUCTION_REMOVE;
    if (checkPermissions(item)) {
        if (item->_isRestoration && item->isDirectory())
            recurse = true;
    } else {
        recurse = false;
    }
    if (recurse) {
        auto job = new ProcessDirectoryJob(path, item, recurseQueryLocal, recurseQueryServer,
            _lastSyncTimestamp, this);
        job->setInsideEncryptedTree(isInsideEncryptedTree() || item->_isEncrypted);
        if (removed) {
            job->setParent(_discoveryData);
            _discoveryData->_queuedDeletedDirectories[path._original] = job;
        } else {
            connect(job, &ProcessDirectoryJob::finished, this, &ProcessDirectoryJob::subJobFinished);
            _queuedJobs.push_back(job);
        }
    } else {
        if (removed
            // For the purpose of rename deletion, restored deleted placeholder is as if it was deleted
            || (item->_type == ItemTypeVirtualFile && item->_instruction == CSYNC_INSTRUCTION_NEW)) {
            _discoveryData->_deletedItem[path._original] = item;
        }
        emit _discoveryData->itemDiscovered(item);
    }
}

void ProcessDirectoryJob::processBlacklisted(const PathTuple &path, const OCC::LocalInfo &localEntry,
    const SyncJournalFileRecord &dbEntry)
{
    if (!localEntry.isValid())
        return;

    auto item = SyncFileItem::fromSyncJournalFileRecord(dbEntry);
    item->_file = path._target;
    item->_originalFile = path._original;
    item->_inode = localEntry.inode;
    item->_isSelectiveSync = true;
    if (dbEntry.isValid() && ((dbEntry._modtime == localEntry.modtime && dbEntry._fileSize == localEntry.size) || (localEntry.isDirectory && dbEntry.isDirectory()))) {
        item->_instruction = CSYNC_INSTRUCTION_REMOVE;
        item->_direction = SyncFileItem::Down;
    } else {
        item->_instruction = CSYNC_INSTRUCTION_IGNORE;
        item->_status = SyncFileItem::FileIgnored;
        item->_errorString = tr("Ignored because of the \"choose what to sync\" blacklist");
        _childIgnored = true;
    }

    qCInfo(lcDisco) << "Discovered (blacklisted) " << item->_file << item->_instruction << item->_direction << item->isDirectory();

    if (item->isDirectory() && item->_instruction != CSYNC_INSTRUCTION_IGNORE) {
        auto job = new ProcessDirectoryJob(path, item, NormalQuery, InBlackList, _lastSyncTimestamp, this);
        connect(job, &ProcessDirectoryJob::finished, this, &ProcessDirectoryJob::subJobFinished);
        _queuedJobs.push_back(job);
    } else {
        emit _discoveryData->itemDiscovered(item);
    }
}

bool ProcessDirectoryJob::checkPermissions(const OCC::SyncFileItemPtr &item)
{
    if (item->_direction != SyncFileItem::Up) {
        // Currently we only check server-side permissions
        return true;
    }

    switch (item->_instruction) {
    case CSYNC_INSTRUCTION_TYPE_CHANGE:
    case CSYNC_INSTRUCTION_NEW: {
        const auto perms = !_rootPermissions.isNull() ? _rootPermissions
                                                      : _dirItem ? _dirItem->_remotePerm : _rootPermissions;
        if (perms.isNull()) {
            // No permissions set
            return true;
        } else if (item->isDirectory() && !perms.hasPermission(RemotePermissions::CanAddSubDirectories)) {
            qCWarning(lcDisco) << "checkForPermission: ERROR" << item->_file;
            item->_instruction = CSYNC_INSTRUCTION_ERROR;
            item->_errorString = tr("Not allowed because you don't have permission to add subfolders to that folder");
            return false;
        } else if (!item->isDirectory() && !perms.hasPermission(RemotePermissions::CanAddFile)) {
            qCWarning(lcDisco) << "checkForPermission: ERROR" << item->_file;
            item->_instruction = CSYNC_INSTRUCTION_ERROR;
            item->_errorString = tr("Not allowed because you don't have permission to add files in that folder");
            return false;
        }
        break;
    }
    case CSYNC_INSTRUCTION_SYNC: {
        const auto perms = item->_remotePerm;
        if (perms.isNull()) {
            // No permissions set
            return true;
        }
        if (!perms.hasPermission(RemotePermissions::CanWrite)) {
            item->_instruction = CSYNC_INSTRUCTION_CONFLICT;
            item->_errorString = tr("Not allowed to upload this file because it is read-only on the server, restoring");
            item->_direction = SyncFileItem::Down;
            item->_isRestoration = true;
            qCWarning(lcDisco) << "checkForPermission: RESTORING" << item->_file << item->_errorString;
            // Take the things to write to the db from the "other" node (i.e: info from server).
            // Do a lookup into the csync remote tree to get the metadata we need to restore.
            qSwap(item->_size, item->_previousSize);
            qSwap(item->_modtime, item->_previousModtime);
            return false;
        }
        break;
    }
    case CSYNC_INSTRUCTION_REMOVE: {
        QString fileSlash = item->_file + '/';
        auto forbiddenIt = _discoveryData->_forbiddenDeletes.upperBound(fileSlash);
        if (forbiddenIt != _discoveryData->_forbiddenDeletes.begin())
            forbiddenIt -= 1;
        if (forbiddenIt != _discoveryData->_forbiddenDeletes.end()
            && fileSlash.startsWith(forbiddenIt.key())) {
            item->_instruction = CSYNC_INSTRUCTION_NEW;
            item->_direction = SyncFileItem::Down;
            item->_isRestoration = true;
            item->_errorString = tr("Moved to invalid target, restoring");
            qCWarning(lcDisco) << "checkForPermission: RESTORING" << item->_file << item->_errorString;
            return true; // restore sub items
        }
        const auto perms = item->_remotePerm;
        if (perms.isNull()) {
            // No permissions set
            return true;
        }
        if (!perms.hasPermission(RemotePermissions::CanDelete)) {
            item->_instruction = CSYNC_INSTRUCTION_NEW;
            item->_direction = SyncFileItem::Down;
            item->_isRestoration = true;
            item->_errorString = tr("Not allowed to remove, restoring");
            qCWarning(lcDisco) << "checkForPermission: RESTORING" << item->_file << item->_errorString;
            return true; // (we need to recurse to restore sub items)
        }
        break;
    }
    default:
        break;
    }
    return true;
}


auto ProcessDirectoryJob::checkMovePermissions(RemotePermissions srcPerm, const QString &srcPath,
                                               bool isDirectory)
    -> MovePermissionResult
{
    auto destPerms = !_rootPermissions.isNull() ? _rootPermissions
                                                : _dirItem ? _dirItem->_remotePerm : _rootPermissions;
    auto filePerms = srcPerm;
    //true when it is just a rename in the same directory. (not a move)
    bool isRename = srcPath.startsWith(_currentFolder._original)
        && srcPath.lastIndexOf('/') == _currentFolder._original.size();
    // Check if we are allowed to move to the destination.
    bool destinationOK = true;
    bool destinationNewOK = true;
    if (destPerms.isNull()) {
    } else if ((isDirectory && !destPerms.hasPermission(RemotePermissions::CanAddSubDirectories)) ||
              (!isDirectory && !destPerms.hasPermission(RemotePermissions::CanAddFile))) {
        destinationNewOK = false;
    }
    if (!isRename && !destinationNewOK) {
        // no need to check for the destination dir permission for renames
        destinationOK = false;
    }

    // check if we are allowed to move from the source
    bool sourceOK = true;
    if (!filePerms.isNull()
        && ((isRename && !filePerms.hasPermission(RemotePermissions::CanRename))
                || (!isRename && !filePerms.hasPermission(RemotePermissions::CanMove)))) {
        // We are not allowed to move or rename this file
        sourceOK = false;
    }
    return MovePermissionResult{sourceOK, destinationOK, destinationNewOK};
}

void ProcessDirectoryJob::subJobFinished()
{
    auto job = qobject_cast<ProcessDirectoryJob *>(sender());
    ASSERT(job);

    _childIgnored |= job->_childIgnored;
    _childModified |= job->_childModified;

    if (job->_dirItem)
        emit _discoveryData->itemDiscovered(job->_dirItem);

    int count = _runningJobs.removeAll(job);
    ASSERT(count == 1);
    job->deleteLater();
    QTimer::singleShot(0, _discoveryData, &DiscoveryPhase::scheduleMoreJobs);
}

int ProcessDirectoryJob::processSubJobs(int nbJobs)
{
    if (_queuedJobs.empty() && _runningJobs.empty() && _pendingAsyncJobs == 0) {
        _pendingAsyncJobs = -1; // We're finished, we don't want to emit finished again
        if (_dirItem) {
            if (_childModified && _dirItem->_instruction == CSYNC_INSTRUCTION_REMOVE) {
                // re-create directory that has modified contents
                _dirItem->_instruction = CSYNC_INSTRUCTION_NEW;
                _dirItem->_direction = _dirItem->_direction == SyncFileItem::Up ? SyncFileItem::Down : SyncFileItem::Up;
            }
            if (_childModified && _dirItem->_instruction == CSYNC_INSTRUCTION_TYPE_CHANGE && !_dirItem->isDirectory()) {
                // Replacing a directory by a file is a conflict, if the directory had modified children
                _dirItem->_instruction = CSYNC_INSTRUCTION_CONFLICT;
                if (_dirItem->_direction == SyncFileItem::Up) {
                    _dirItem->_type = ItemTypeDirectory;
                    _dirItem->_direction = SyncFileItem::Down;
                }
            }
            if (_childIgnored && _dirItem->_instruction == CSYNC_INSTRUCTION_REMOVE) {
                // Do not remove a directory that has ignored files
                _dirItem->_instruction = CSYNC_INSTRUCTION_NONE;
            }
        }
        emit finished();
    }

    int started = 0;
    foreach (auto *rj, _runningJobs) {
        started += rj->processSubJobs(nbJobs - started);
        if (started >= nbJobs)
            return started;
    }

    while (started < nbJobs && !_queuedJobs.empty()) {
        auto f = _queuedJobs.front();
        _queuedJobs.pop_front();
        _runningJobs.push_back(f);
        f->start();
        started++;
    }
    return started;
}

void ProcessDirectoryJob::dbError()
{
    _discoveryData->fatalError(tr("Error while reading the database"));
}

void ProcessDirectoryJob::addVirtualFileSuffix(QString &str) const
{
    str.append(_discoveryData->_syncOptions._vfs->fileSuffix());
}

bool ProcessDirectoryJob::hasVirtualFileSuffix(const QString &str) const
{
    if (!isVfsWithSuffix())
        return false;
    return str.endsWith(_discoveryData->_syncOptions._vfs->fileSuffix());
}

void ProcessDirectoryJob::chopVirtualFileSuffix(QString &str) const
{
    if (!isVfsWithSuffix())
        return;
    bool hasSuffix = hasVirtualFileSuffix(str);
    ASSERT(hasSuffix);
    if (hasSuffix)
        str.chop(_discoveryData->_syncOptions._vfs->fileSuffix().size());
}

DiscoverySingleDirectoryJob *ProcessDirectoryJob::startAsyncServerQuery()
{
    auto serverJob = new DiscoverySingleDirectoryJob(_discoveryData->_account,
        _discoveryData->_remoteFolder + _currentFolder._server, this);
    if (!_dirItem)
        serverJob->setIsRootPath(); // query the fingerprint on the root
    connect(serverJob, &DiscoverySingleDirectoryJob::etag, this, &ProcessDirectoryJob::etag);
    _discoveryData->_currentlyActiveJobs++;
    _pendingAsyncJobs++;
    connect(serverJob, &DiscoverySingleDirectoryJob::finished, this, [this, serverJob](const auto &results) {
        _discoveryData->_currentlyActiveJobs--;
        _pendingAsyncJobs--;
        if (results) {
            _serverNormalQueryEntries = *results;
            _serverQueryDone = true;
            if (!serverJob->_dataFingerprint.isEmpty() && _discoveryData->_dataFingerprint.isEmpty())
                _discoveryData->_dataFingerprint = serverJob->_dataFingerprint;
            if (_localQueryDone)
                this->process();
        } else {
            auto code = results.error().code;
            qCWarning(lcDisco) << "Server error in directory" << _currentFolder._server << code;
            if (_dirItem && code >= 403) {
                // In case of an HTTP error, we ignore that directory
                // 403 Forbidden can be sent by the server if the file firewall is active.
                // A file or directory should be ignored and sync must continue. See #3490
                // The server usually replies with the custom "503 Storage not available"
                // if some path is temporarily unavailable. But in some cases a standard 503
                // is returned too. Thus we can't distinguish the two and will treat any
                // 503 as request to ignore the folder. See #3113 #2884.
                // Similarly, the server might also return 404 or 50x in case of bugs. #7199 #7586
                _dirItem->_instruction = CSYNC_INSTRUCTION_IGNORE;
                _dirItem->_errorString = results.error().message;
                emit this->finished();
            } else {
                // Fatal for the root job since it has no SyncFileItem, or for the network errors
                emit _discoveryData->fatalError(tr("Server replied with an error while reading directory '%1' : %2")
                    .arg(_currentFolder._server, results.error().message));
            }
        }
    });
    connect(serverJob, &DiscoverySingleDirectoryJob::firstDirectoryPermissions, this,
        [this](const RemotePermissions &perms) { _rootPermissions = perms; });
    serverJob->start();
    return serverJob;
}

void ProcessDirectoryJob::startAsyncLocalQuery()
{
    QString localPath = _discoveryData->_localDir + _currentFolder._local;
    auto localJob = new DiscoverySingleLocalDirectoryJob(_discoveryData->_account, localPath, _discoveryData->_syncOptions._vfs.data());

    _discoveryData->_currentlyActiveJobs++;
    _pendingAsyncJobs++;

    connect(localJob, &DiscoverySingleLocalDirectoryJob::itemDiscovered, _discoveryData, &DiscoveryPhase::itemDiscovered);

    connect(localJob, &DiscoverySingleLocalDirectoryJob::childIgnored, this, [this](bool b) {
        _childIgnored = b;
    });

    connect(localJob, &DiscoverySingleLocalDirectoryJob::finishedFatalError, this, [this](const QString &msg) {
        _discoveryData->_currentlyActiveJobs--;
        _pendingAsyncJobs--;
        if (_serverJob)
            _serverJob->abort();

        emit _discoveryData->fatalError(msg);
    });

    connect(localJob, &DiscoverySingleLocalDirectoryJob::finishedNonFatalError, this, [this](const QString &msg) {
        _discoveryData->_currentlyActiveJobs--;
        _pendingAsyncJobs--;

        if (_dirItem) {
            _dirItem->_instruction = CSYNC_INSTRUCTION_IGNORE;
            _dirItem->_errorString = msg;
            emit this->finished();
        } else {
            // Fatal for the root job since it has no SyncFileItem
            emit _discoveryData->fatalError(msg);
        }
    });

    connect(localJob, &DiscoverySingleLocalDirectoryJob::finished, this, [this](const auto &results) {
        _discoveryData->_currentlyActiveJobs--;
        _pendingAsyncJobs--;

        _localNormalQueryEntries = results;
        _localQueryDone = true;

        if (_serverQueryDone)
            this->process();
    });

    QThreadPool *pool = QThreadPool::globalInstance();
    pool->start(localJob); // QThreadPool takes ownership
}


bool ProcessDirectoryJob::isVfsWithSuffix() const
{
    return _discoveryData->_syncOptions._vfs->mode() == Vfs::WithSuffix;
}

void ProcessDirectoryJob::computePinState(PinState parentState)
{
    _pinState = parentState;
    if (_queryLocal != ParentDontExist) {
        if (auto state = _discoveryData->_syncOptions._vfs->pinState(_currentFolder._local)) // ouch! pin local or original?
            _pinState = *state;
    }
}

void ProcessDirectoryJob::setupDbPinStateActions(SyncJournalFileRecord &record)
{
    // Only suffix-vfs uses the db for pin states.
    // Other plugins will set localEntry._type according to the file's pin state.
    if (!isVfsWithSuffix())
        return;

    auto pin = _discoveryData->_statedb->internalPinStates().rawForPath(record._path);
    if (!pin || *pin == PinState::Inherited)
        pin = _pinState;

    // OnlineOnly hydrated files want to be dehydrated
    if (record._type == ItemTypeFile && *pin == PinState::OnlineOnly)
        record._type = ItemTypeVirtualFileDehydration;

    // AlwaysLocal dehydrated files want to be hydrated
    if (record._type == ItemTypeVirtualFile && *pin == PinState::AlwaysLocal)
        record._type = ItemTypeVirtualFileDownload;
}

}
