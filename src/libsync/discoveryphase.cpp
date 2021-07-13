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

#include "discoveryphase.h"
#include "discovery.h"

#include "account.h"
#include "clientsideencryptionjobs.h"

#include "common/asserts.h"
#include "common/checksums.h"

#include <csync_exclude.h>
#include "vio/csync_vio_local.h"

#include <QLoggingCategory>
#include <QUrl>
#include <QFile>
#include <QFileInfo>
#include <QTextCodec>
#include <cstring>
#include <QDateTime>


namespace OCC {

Q_LOGGING_CATEGORY(lcDiscovery, "nextcloud.sync.discovery", QtInfoMsg)

/* Given a sorted list of paths ending with '/', return whether or not the given path is within one of the paths of the list*/
static bool findPathInList(const QStringList &list, const QString &path)
{
    Q_ASSERT(std::is_sorted(list.begin(), list.end()));

    if (list.size() == 1 && list.first() == QLatin1String("/")) {
        // Special case for the case "/" is there, it matches everything
        return true;
    }

    QString pathSlash = path + QLatin1Char('/');

    // Since the list is sorted, we can do a binary search.
    // If the path is a prefix of another item or right after in the lexical order.
    auto it = std::lower_bound(list.begin(), list.end(), pathSlash);

    if (it != list.end() && *it == pathSlash) {
        return true;
    }

    if (it == list.begin()) {
        return false;
    }
    --it;
    Q_ASSERT(it->endsWith(QLatin1Char('/'))); // Folder::setSelectiveSyncBlackList makes sure of that
    return pathSlash.startsWith(*it);
}

bool DiscoveryPhase::isInSelectiveSyncBlackList(const QString &path) const
{
    if (_selectiveSyncBlackList.isEmpty()) {
        // If there is no black list, everything is allowed
        return false;
    }

    // Block if it is in the black list
    if (findPathInList(_selectiveSyncBlackList, path)) {
        return true;
    }

    return false;
}

void DiscoveryPhase::checkSelectiveSyncNewFolder(const QString &path, RemotePermissions remotePerm,
    std::function<void(bool)> callback)
{
    if (_syncOptions._confirmExternalStorage && _syncOptions._vfs->mode() == Vfs::Off
        && remotePerm.hasPermission(RemotePermissions::IsMounted)) {
        // external storage.

        /* Note: DiscoverySingleDirectoryJob::directoryListingIteratedSlot make sure that only the
         * root of a mounted storage has 'M', all sub entries have 'm' */

        // Only allow it if the white list contains exactly this path (not parents)
        // We want to ask confirmation for external storage even if the parents where selected
        if (_selectiveSyncWhiteList.contains(path + QLatin1Char('/'))) {
            return callback(false);
        }

        emit newBigFolder(path, true);
        return callback(true);
    }

    // If this path or the parent is in the white list, then we do not block this file
    if (findPathInList(_selectiveSyncWhiteList, path)) {
        return callback(false);
    }

    auto limit = _syncOptions._newBigFolderSizeLimit;
    if (limit < 0 || _syncOptions._vfs->mode() != Vfs::Off) {
        // no limit, everything is allowed;
        return callback(false);
    }

    // do a PROPFIND to know the size of this folder
    auto propfindJob = new PropfindJob(_account, _remoteFolder + path, this);
    propfindJob->setProperties(QList<QByteArray>() << "resourcetype"
                                                   << "http://owncloud.org/ns:size");
    QObject::connect(propfindJob, &PropfindJob::finishedWithError,
        this, [=] { return callback(false); });
    QObject::connect(propfindJob, &PropfindJob::result, this, [=](const QVariantMap &values) {
        auto result = values.value(QLatin1String("size")).toLongLong();
        if (result >= limit) {
            // we tell the UI there is a new folder
            emit newBigFolder(path, false);
            return callback(true);
        } else {
            // it is not too big, put it in the white list (so we will not do more query for the children)
            // and and do not block.
            auto p = path;
            if (!p.endsWith(QLatin1Char('/')))
                p += QLatin1Char('/');
            _selectiveSyncWhiteList.insert(
                std::upper_bound(_selectiveSyncWhiteList.begin(), _selectiveSyncWhiteList.end(), p),
                p);
            return callback(false);
        }
    });
    propfindJob->start();
}

/* Given a path on the remote, give the path as it is when the rename is done */
QString DiscoveryPhase::adjustRenamedPath(const QString &original, SyncFileItem::Direction d) const
{
    return OCC::adjustRenamedPath(d == SyncFileItem::Down ? _renamedItemsRemote : _renamedItemsLocal, original);
}

QString adjustRenamedPath(const QMap<QString, QString> &renamedItems, const QString &original)
{
    int slashPos = original.size();
    while ((slashPos = original.lastIndexOf('/', slashPos - 1)) > 0) {
        auto it = renamedItems.constFind(original.left(slashPos));
        if (it != renamedItems.constEnd()) {
            return *it + original.mid(slashPos);
        }
    }
    return original;
}

QPair<bool, QByteArray> DiscoveryPhase::findAndCancelDeletedJob(const QString &originalPath)
{
    bool result = false;
    QByteArray oldEtag;
    auto it = _deletedItem.find(originalPath);
    if (it != _deletedItem.end()) {
        const SyncInstructions instruction = (*it)->_instruction;
        if (instruction == CSYNC_INSTRUCTION_IGNORE && (*it)->_type == ItemTypeVirtualFile) {
            // re-creation of virtual files count as a delete
            // a file might be in an error state and thus gets marked as CSYNC_INSTRUCTION_IGNORE
            // after it was initially marked as CSYNC_INSTRUCTION_REMOVE
            // return true, to not trigger any additional actions on that file that could elad to dataloss
            result = true;
            oldEtag = (*it)->_etag;
        } else {
            if (!(instruction == CSYNC_INSTRUCTION_REMOVE
                    // re-creation of virtual files count as a delete
                    || ((*it)->_type == ItemTypeVirtualFile && instruction == CSYNC_INSTRUCTION_NEW)
                    || ((*it)->_isRestoration && instruction == CSYNC_INSTRUCTION_NEW)))
            {
                qCWarning(lcDiscovery) << "ENFORCE(FAILING)" << originalPath;
                qCWarning(lcDiscovery) << "instruction == CSYNC_INSTRUCTION_REMOVE" << (instruction == CSYNC_INSTRUCTION_REMOVE);
                qCWarning(lcDiscovery) << "((*it)->_type == ItemTypeVirtualFile && instruction == CSYNC_INSTRUCTION_NEW)"
                                       << ((*it)->_type == ItemTypeVirtualFile && instruction == CSYNC_INSTRUCTION_NEW);
                qCWarning(lcDiscovery) << "((*it)->_isRestoration && instruction == CSYNC_INSTRUCTION_NEW))"
                                       << ((*it)->_isRestoration && instruction == CSYNC_INSTRUCTION_NEW);
                qCWarning(lcDiscovery) << "instruction" << instruction;
                qCWarning(lcDiscovery) << "(*it)->_type" << (*it)->_type;
                qCWarning(lcDiscovery) << "(*it)->_isRestoration " << (*it)->_isRestoration;
                ENFORCE(false);
            }
            (*it)->_instruction = CSYNC_INSTRUCTION_NONE;
            result = true;
            oldEtag = (*it)->_etag;
        }
        _deletedItem.erase(it);
    }
    if (auto *otherJob = _queuedDeletedDirectories.take(originalPath)) {
        oldEtag = otherJob->_dirItem->_etag;
        delete otherJob;
        result = true;
    }
    return { result, oldEtag };
}

void DiscoveryPhase::startJob(ProcessDirectoryJob *job)
{
    ENFORCE(!_currentRootJob);
    connect(job, &ProcessDirectoryJob::finished, this, [this, job] {
        ENFORCE(_currentRootJob == sender());
        _currentRootJob = nullptr;
        if (job->_dirItem)
            emit itemDiscovered(job->_dirItem);
        job->deleteLater();

        // Once the main job has finished recurse here to execute the remaining
        // jobs for queued deleted directories.
        if (!_queuedDeletedDirectories.isEmpty()) {
            auto nextJob = _queuedDeletedDirectories.take(_queuedDeletedDirectories.firstKey());
            startJob(nextJob);
        } else {
            emit finished();
        }
    });
    _currentRootJob = job;
    job->start();
}

void DiscoveryPhase::setSelectiveSyncBlackList(const QStringList &list)
{
    _selectiveSyncBlackList = list;
    std::sort(_selectiveSyncBlackList.begin(), _selectiveSyncBlackList.end());
}

void DiscoveryPhase::setSelectiveSyncWhiteList(const QStringList &list)
{
    _selectiveSyncWhiteList = list;
    std::sort(_selectiveSyncWhiteList.begin(), _selectiveSyncWhiteList.end());
}

void DiscoveryPhase::scheduleMoreJobs()
{
    auto limit = qMax(1, _syncOptions._parallelNetworkJobs);
    if (_currentRootJob && _currentlyActiveJobs < limit) {
        _currentRootJob->processSubJobs(limit - _currentlyActiveJobs);
    }
}

DiscoverySingleLocalDirectoryJob::DiscoverySingleLocalDirectoryJob(const AccountPtr &account, const QString &localPath, OCC::Vfs *vfs, QObject *parent)
 : QObject(parent), QRunnable(), _localPath(localPath), _account(account), _vfs(vfs)
{
    qRegisterMetaType<QVector<LocalInfo> >("QVector<LocalInfo>");
}

// Use as QRunnable
void DiscoverySingleLocalDirectoryJob::run() {
    QString localPath = _localPath;
    if (localPath.endsWith('/')) // Happens if _currentFolder._local.isEmpty()
        localPath.chop(1);

    auto dh = csync_vio_local_opendir(localPath);
    if (!dh) {
        qCInfo(lcDiscovery) << "Error while opening directory" << (localPath) << errno;
        QString errorString = tr("Error while opening directory %1").arg(localPath);
        if (errno == EACCES) {
            errorString = tr("Directory not accessible on client, permission denied");
            emit finishedNonFatalError(errorString);
            return;
        } else if (errno == ENOENT) {
            errorString = tr("Directory not found: %1").arg(localPath);
        } else if (errno == ENOTDIR) {
            // Not a directory..
            // Just consider it is empty
            return;
        }
        emit finishedFatalError(errorString);
        return;
    }

    QVector<LocalInfo> results;
    while (true) {
        errno = 0;
        auto dirent = csync_vio_local_readdir(dh, _vfs);
        if (!dirent)
            break;
        if (dirent->type == ItemTypeSkip)
            continue;
        LocalInfo i;
        static QTextCodec *codec = QTextCodec::codecForName("UTF-8");
        ASSERT(codec);
        QTextCodec::ConverterState state;
        i.name = codec->toUnicode(dirent->path, dirent->path.size(), &state);
        if (state.invalidChars > 0 || state.remainingChars > 0) {
            emit childIgnored(true);
            auto item = SyncFileItemPtr::create();
            //item->_file = _currentFolder._target + i.name;
            // FIXME ^^ do we really need to use _target or is local fine?
            item->_file = _localPath + i.name;
            item->_instruction = CSYNC_INSTRUCTION_IGNORE;
            item->_status = SyncFileItem::NormalError;
            item->_errorString = tr("Filename encoding is not valid");
            emit itemDiscovered(item);
            continue;
        }
        i.modtime = dirent->modtime;
        i.size = dirent->size;
        i.inode = dirent->inode;
        i.isDirectory = dirent->type == ItemTypeDirectory;
        i.isHidden = dirent->is_hidden;
        i.isSymLink = dirent->type == ItemTypeSoftLink;
        i.isVirtualFile = dirent->type == ItemTypeVirtualFile || dirent->type == ItemTypeVirtualFileDownload;
        i.type = dirent->type;
        results.push_back(i);
    }
    if (errno != 0) {
        csync_vio_local_closedir(dh);

        // Note: Windows vio converts any error into EACCES
        qCWarning(lcDiscovery) << "readdir failed for file in " << localPath << " - errno: " << errno;
        emit finishedFatalError(tr("Error while reading directory %1").arg(localPath));
        return;
    }

    errno = 0;
    csync_vio_local_closedir(dh);
    if (errno != 0) {
        qCWarning(lcDiscovery) << "closedir failed for file in " << localPath << " - errno: " << errno;
    }

    emit finished(results);
}

DiscoverySingleDirectoryJob::DiscoverySingleDirectoryJob(const AccountPtr &account, const QString &path, QObject *parent)
    : QObject(parent)
    , _subPath(path)
    , _account(account)
    , _ignoredFirst(false)
    , _isRootPath(false)
    , _isExternalStorage(false)
    , _isE2eEncrypted(false)
{
}

void DiscoverySingleDirectoryJob::start()
{
    // Start the actual HTTP job
    auto *lsColJob = new LsColJob(_account, _subPath, this);

    QList<QByteArray> props;
    props << "resourcetype"
          << "getlastmodified"
          << "getcontentlength"
          << "getetag"
          << "http://owncloud.org/ns:size"
          << "http://owncloud.org/ns:id"
          << "http://owncloud.org/ns:downloadURL"
          << "http://owncloud.org/ns:dDC"
          << "http://owncloud.org/ns:permissions"
          << "http://owncloud.org/ns:checksums";
    if (_isRootPath)
        props << "http://owncloud.org/ns:data-fingerprint";
    if (_account->serverVersionInt() >= Account::makeServerVersion(10, 0, 0)) {
        // Server older than 10.0 have performances issue if we ask for the share-types on every PROPFIND
        props << "http://owncloud.org/ns:share-types";
    }
    if (_account->capabilities().clientSideEncryptionAvailable()) {
        props << "http://nextcloud.org/ns:is-encrypted";
    }

    lsColJob->setProperties(props);

    QObject::connect(lsColJob, &LsColJob::directoryListingIterated,
        this, &DiscoverySingleDirectoryJob::directoryListingIteratedSlot);
    QObject::connect(lsColJob, &LsColJob::finishedWithError, this, &DiscoverySingleDirectoryJob::lsJobFinishedWithErrorSlot);
    QObject::connect(lsColJob, &LsColJob::finishedWithoutError, this, &DiscoverySingleDirectoryJob::lsJobFinishedWithoutErrorSlot);
    lsColJob->start();

    _lsColJob = lsColJob;
}

void DiscoverySingleDirectoryJob::abort()
{
    if (_lsColJob && _lsColJob->reply()) {
        _lsColJob->reply()->abort();
    }
}

static void propertyMapToRemoteInfo(const QMap<QString, QString> &map, RemoteInfo &result)
{
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        QString property = it.key();
        QString value = it.value();
        if (property == QLatin1String("resourcetype")) {
            result.isDirectory = value.contains(QLatin1String("collection"));
        } else if (property == QLatin1String("getlastmodified")) {
            const auto date = QDateTime::fromString(value, Qt::RFC2822Date);
            Q_ASSERT(date.isValid());
            result.modtime = date.toTime_t();
        } else if (property == QLatin1String("getcontentlength")) {
            // See #4573, sometimes negative size values are returned
            bool ok = false;
            qlonglong ll = value.toLongLong(&ok);
            if (ok && ll >= 0) {
                result.size = ll;
            } else {
                result.size = 0;
            }
        } else if (property == "getetag") {
            result.etag = Utility::normalizeEtag(value.toUtf8());
        } else if (property == "id") {
            result.fileId = value.toUtf8();
        } else if (property == "downloadURL") {
            result.directDownloadUrl = value;
        } else if (property == "dDC") {
            result.directDownloadCookies = value;
        } else if (property == "permissions") {
            result.remotePerm = RemotePermissions::fromServerString(value);
        } else if (property == "checksums") {
            result.checksumHeader = findBestChecksum(value.toUtf8());
        } else if (property == "share-types" && !value.isEmpty()) {
            // Since QMap is sorted, "share-types" is always after "permissions".
            if (result.remotePerm.isNull()) {
                qWarning() << "Server returned a share type, but no permissions?";
            } else {
                // S means shared with me.
                // But for our purpose, we want to know if the file is shared. It does not matter
                // if we are the owner or not.
                // Piggy back on the persmission field
                result.remotePerm.setPermission(RemotePermissions::IsShared);
            }
        } else if (property == "is-encrypted" && value == QStringLiteral("1")) {
            result.isE2eEncrypted = true;
        }
    }

    if (result.isDirectory && map.contains("size")) {
        result.sizeOfFolder = map.value("size").toInt();
    }
}

void DiscoverySingleDirectoryJob::directoryListingIteratedSlot(const QString &file, const QMap<QString, QString> &map)
{
    if (!_ignoredFirst) {
        // The first entry is for the folder itself, we should process it differently.
        _ignoredFirst = true;
        if (map.contains("permissions")) {
            auto perm = RemotePermissions::fromServerString(map.value("permissions"));
            emit firstDirectoryPermissions(perm);
            _isExternalStorage = perm.hasPermission(RemotePermissions::IsMounted);
        }
        if (map.contains("data-fingerprint")) {
            _dataFingerprint = map.value("data-fingerprint").toUtf8();
            if (_dataFingerprint.isEmpty()) {
                // Placeholder that means that the server supports the feature even if it did not set one.
                _dataFingerprint = "[empty]";
            }
        }
        if (map.contains("id")) {
            _fileId = map.value("id").toUtf8();
        }
        if (map.contains("is-encrypted") && map.value("is-encrypted") == QStringLiteral("1")) {
            _isE2eEncrypted = true;
            Q_ASSERT(!_fileId.isEmpty());
        }
        if (map.contains("size")) {
            _size = map.value("size").toInt();
        }
    } else {

        RemoteInfo result;
        int slash = file.lastIndexOf('/');
        result.name = file.mid(slash + 1);
        result.size = -1;
        propertyMapToRemoteInfo(map, result);
        if (result.isDirectory)
            result.size = 0;

        if (_isExternalStorage && result.remotePerm.hasPermission(RemotePermissions::IsMounted)) {
            /* All the entries in a external storage have 'M' in their permission. However, for all
               purposes in the desktop client, we only need to know about the mount points.
               So replace the 'M' by a 'm' for every sub entries in an external storage */
            result.remotePerm.unsetPermission(RemotePermissions::IsMounted);
            result.remotePerm.setPermission(RemotePermissions::IsMountedSub);
        }

        QStringRef fileRef(&file);
        int slashPos = file.lastIndexOf(QLatin1Char('/'));
        if (slashPos > -1) {
            fileRef = file.midRef(slashPos + 1);
        }
        _results.push_back(std::move(result));
    }

    //This works in concerto with the RequestEtagJob and the Folder object to check if the remote folder changed.
    if (map.contains("getetag")) {
        if (_firstEtag.isEmpty()) {
            _firstEtag = parseEtag(map.value("getetag").toUtf8()); // for directory itself
        }
    }
}

void DiscoverySingleDirectoryJob::lsJobFinishedWithoutErrorSlot()
{
    if (!_ignoredFirst) {
        // This is a sanity check, if we haven't _ignoredFirst then it means we never received any directoryListingIteratedSlot
        // which means somehow the server XML was bogus
        emit finished(HttpError{ 0, tr("Server error: PROPFIND reply is not XML formatted!") });
        deleteLater();
        return;
    } else if (!_error.isEmpty()) {
        emit finished(HttpError{ 0, _error });
        deleteLater();
        return;
    } else if (_isE2eEncrypted) {
        emit etag(_firstEtag, QDateTime::fromString(QString::fromUtf8(_lsColJob->responseTimestamp()), Qt::RFC2822Date));
        fetchE2eMetadata();
        return;
    }
    emit etag(_firstEtag, QDateTime::fromString(QString::fromUtf8(_lsColJob->responseTimestamp()), Qt::RFC2822Date));
    emit finished(_results);
    deleteLater();
}

void DiscoverySingleDirectoryJob::lsJobFinishedWithErrorSlot(QNetworkReply *r)
{
    QString contentType = r->header(QNetworkRequest::ContentTypeHeader).toString();
    int httpCode = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString msg = r->errorString();
    qCWarning(lcDiscovery) << "LSCOL job error" << r->errorString() << httpCode << r->error();
    if (r->error() == QNetworkReply::NoError
        && !contentType.contains("application/xml; charset=utf-8")) {
        msg = tr("Server error: PROPFIND reply is not XML formatted!");
    }
    emit finished(HttpError{ httpCode, msg });
    deleteLater();
}

void DiscoverySingleDirectoryJob::fetchE2eMetadata()
{
    auto job = new GetMetadataApiJob(_account, _fileId);
    connect(job, &GetMetadataApiJob::jsonReceived,
            this, &DiscoverySingleDirectoryJob::metadataReceived);
    connect(job, &GetMetadataApiJob::error,
            this, &DiscoverySingleDirectoryJob::metadataError);
    job->start();
}

void DiscoverySingleDirectoryJob::metadataReceived(const QJsonDocument &json, int statusCode)
{
    qCDebug(lcDiscovery) << "Metadata received, applying it to the result list";
    Q_ASSERT(_subPath.startsWith('/'));

    const auto metadata = FolderMetadata(_account, json.toJson(QJsonDocument::Compact), statusCode);
    const auto encryptedFiles = metadata.files();

    const auto findEncryptedFile = [=](const QString &name) {
        const auto it = std::find_if(std::cbegin(encryptedFiles), std::cend(encryptedFiles), [=](const EncryptedFile &file) {
            return file.encryptedFilename == name;
        });
        if (it == std::cend(encryptedFiles)) {
            return Optional<EncryptedFile>();
        } else {
            return Optional<EncryptedFile>(*it);
        }
    };

    std::transform(std::cbegin(_results), std::cend(_results), std::begin(_results), [=](const RemoteInfo &info) {
        auto result = info;
        const auto encryptedFileInfo = findEncryptedFile(result.name);
        if (encryptedFileInfo) {
            result.isE2eEncrypted = true;
            result.e2eMangledName = _subPath.mid(1) + QLatin1Char('/') + result.name;
            result.name = encryptedFileInfo->originalFilename;
        }
        return result;
    });

    emit finished(_results);
    deleteLater();
}

void DiscoverySingleDirectoryJob::metadataError(const QByteArray &fileId, int httpReturnCode)
{
    qCWarning(lcDiscovery) << "E2EE Metadata job error. Trying to proceed without it." << fileId << httpReturnCode;
    emit finished(_results);
    deleteLater();
}
}
