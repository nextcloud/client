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

#include "account.h"
#include "common/asserts.h"
#include "common/checksums.h"

#include <csync_private.h>
#include <csync_rename.h>
#include <csync_exclude.h>

#include <QLoggingCategory>
#include <QUrl>
#include <QFileInfo>
#include <cstring>


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

bool DiscoveryJob::isInSelectiveSyncBlackList(const QByteArray &path) const
{
    if (_selectiveSyncBlackList.isEmpty()) {
        // If there is no black list, everything is allowed
        return false;
    }

    // Block if it is in the black list
    if (findPathInList(_selectiveSyncBlackList, QString::fromUtf8(path))) {
        return true;
    }

    // Also try to adjust the path if there was renames
    if (csync_rename_count(_csync_ctx)) {
        QByteArray adjusted = csync_rename_adjust_parent_path_source(_csync_ctx, path);
        if (adjusted != path) {
            return findPathInList(_selectiveSyncBlackList, QString::fromUtf8(adjusted));
        }
    }

    return false;
}

int DiscoveryJob::isInSelectiveSyncBlackListCallback(void *data, const QByteArray &path)
{
    return static_cast<DiscoveryJob *>(data)->isInSelectiveSyncBlackList(path);
}

bool DiscoveryJob::checkSelectiveSyncNewObject(const QString &path, RemotePermissions remotePerm, const bool &isDirectory)
{
    if (_syncOptions._confirmExternalStorage
        && remotePerm.hasPermission(RemotePermissions::IsMounted)) {
        // external storage.

        /* Note: DiscoverySingleDirectoryJob::directoryListingIteratedSlot make sure that only the
         * root of a mounted storage has 'M', all sub entries have 'm' */

        // Only allow it if the white list contains exactly this path (not parents)
        // We want to ask confirmation for external storage even if the parents where selected
        if (_selectiveSyncWhiteList.contains(path + QLatin1Char('/'))) {
            return false;
        }

        emit newBigObject(path, true, isDirectory);
        return true;
    }

    // If this path or the parent is in the white list, then we do not block this file
    if (findPathInList(_selectiveSyncWhiteList, path)) {
        return false;
    }

    auto limit = _syncOptions._newBigFolderSizeLimit;
    if (limit < 0) {
        // no limit, everything is allowed;
        return false;
    }

    // Go in the main thread to do a PROPFIND to know the size of this folder
    qint64 result = -1;

    {
        QMutexLocker locker(&_vioMutex);
        emit doGetSizeSignal(path, &result);
        _vioWaitCondition.wait(&_vioMutex);
    }

    if (result >= limit) {
        // we tell the UI there is a new object
        emit newBigObject(path, false, isDirectory);
        return true;
    } else {
        // it is not too big, put it in the white list (so we will not do more query for the children)
        // and and do not block.
        auto p = path;
        if (!p.endsWith(QLatin1Char('/'))) {
            p += QLatin1Char('/');
        }
        _selectiveSyncWhiteList.insert(std::upper_bound(_selectiveSyncWhiteList.begin(),
                                           _selectiveSyncWhiteList.end(), p),
            p);

        return false;
    }
}

int DiscoveryJob::checkSelectiveSyncNewObjectCallback(void *data, const QByteArray &path, RemotePermissions remotePerm, const bool &isDirectory)
{
    return static_cast<DiscoveryJob *>(data)->checkSelectiveSyncNewObject(QString::fromUtf8(path), remotePerm, isDirectory);
}


void DiscoveryJob::update_job_update_callback(bool local,
    const char *dirUrl,
    void *userdata)
{
    DiscoveryJob *updateJob = static_cast<DiscoveryJob *>(userdata);
    if (updateJob) {
        // Don't wanna overload the UI
        if (!updateJob->_lastUpdateProgressCallbackCall.isValid()) {
            updateJob->_lastUpdateProgressCallbackCall.start(); // first call
        } else if (updateJob->_lastUpdateProgressCallbackCall.elapsed() < 200) {
            return;
        } else {
            updateJob->_lastUpdateProgressCallbackCall.start();
        }

        QByteArray pPath(dirUrl);
        int indx = pPath.lastIndexOf('/');
        if (indx > -1) {
            const QString path = QUrl::fromPercentEncoding(pPath.mid(indx + 1));
            emit updateJob->folderDiscovered(local, path);
        }
    }
}

// Only use for error cases! It will always set an error errno
int get_errno_from_http_errcode(int err, const QString &reason)
{
    int new_errno = EIO;

    switch (err) {
    case 401: /* Unauthorized */
    case 402: /* Payment Required */
    case 407: /* Proxy Authentication Required */
    case 405:
        new_errno = EPERM;
        break;
    case 301: /* Moved Permanently */
    case 303: /* See Other */
    case 404: /* Not Found */
    case 410: /* Gone */
        new_errno = ENOENT;
        break;
    case 408: /* Request Timeout */
    case 504: /* Gateway Timeout */
        new_errno = EAGAIN;
        break;
    case 423: /* Locked */
        new_errno = EACCES;
        break;
    case 403: /* Forbidden */
        new_errno = ERRNO_FORBIDDEN;
        break;
    case 400: /* Bad Request */
    case 409: /* Conflict */
    case 411: /* Length Required */
    case 412: /* Precondition Failed */
    case 414: /* Request-URI Too Long */
    case 415: /* Unsupported Media Type */
    case 424: /* Failed Dependency */
    case 501: /* Not Implemented */
        new_errno = EINVAL;
        break;
    case 507: /* Insufficient Storage */
        new_errno = ENOSPC;
        break;
    case 206: /* Partial Content */
    case 300: /* Multiple Choices */
    case 302: /* Found */
    case 305: /* Use Proxy */
    case 306: /* (Unused) */
    case 307: /* Temporary Redirect */
    case 406: /* Not Acceptable */
    case 416: /* Requested Range Not Satisfiable */
    case 417: /* Expectation Failed */
    case 422: /* Unprocessable Entity */
    case 500: /* Internal Server Error */
    case 502: /* Bad Gateway */
    case 505: /* HTTP Version Not Supported */
        new_errno = EIO;
        break;
    case 503: /* Service Unavailable */
        // https://github.com/owncloud/core/pull/26145/files
        if (reason == "Storage not available" || reason == "Storage is temporarily not available") {
            new_errno = ERRNO_STORAGE_UNAVAILABLE;
        } else {
            new_errno = ERRNO_SERVICE_UNAVAILABLE;
        }
        break;
    case 413: /* Request Entity too Large */
        new_errno = EFBIG;
        break;
    default:
        new_errno = EIO;
    }
    return new_errno;
}


DiscoverySingleDirectoryJob::DiscoverySingleDirectoryJob(const AccountPtr &account, const QString &path, QObject *parent)
    : QObject(parent)
    , _subPath(path)
    , _account(account)
    , _ignoredFirst(false)
    , _isRootPath(false)
    , _isExternalStorage(false)
{
}

void DiscoverySingleDirectoryJob::start()
{
    // Start the actual HTTP job
    LsColJob *lsColJob = new LsColJob(_account, _subPath, this);

    QList<QByteArray> props;
    props << "resourcetype"
          << "getlastmodified"
          << "getcontentlength"
          << "getetag"
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

static void propertyMapToFileStat(const QMap<QString, QString> &map, csync_file_stat_t *file_stat)
{
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        QString property = it.key();
        QString value = it.value();
        if (property == "resourcetype") {
            if (value.contains("collection")) {
                file_stat->type = ItemTypeDirectory;
            } else {
                file_stat->type = ItemTypeFile;
            }
        } else if (property == "getlastmodified") {
            file_stat->modtime = oc_httpdate_parse(value.toUtf8());
        } else if (property == "getcontentlength") {
            // See #4573, sometimes negative size values are returned
            bool ok = false;
            qlonglong ll = value.toLongLong(&ok);
            if (ok && ll >= 0) {
                file_stat->size = ll;
            } else {
                file_stat->size = 0;
            }
        } else if (property == "getetag") {
            file_stat->etag = Utility::normalizeEtag(value.toUtf8());
        } else if (property == "id") {
            file_stat->file_id = value.toUtf8();
        } else if (property == "downloadURL") {
            file_stat->directDownloadUrl = value.toUtf8();
        } else if (property == "dDC") {
            file_stat->directDownloadCookies = value.toUtf8();
        } else if (property == "permissions") {
            file_stat->remotePerm = RemotePermissions(value);
        } else if (property == "checksums") {
            file_stat->checksumHeader = findBestChecksum(value.toUtf8());
        } else if (property == "share-types" && !value.isEmpty()) {
            // Since QMap is sorted, "share-types" is always after "permissions".
            if (file_stat->remotePerm.isNull()) {
                qWarning() << "Server returned a share type, but no permissions?";
            } else {
                // S means shared with me.
                // But for our purpose, we want to know if the file is shared. It does not matter
                // if we are the owner or not.
                // Piggy back on the persmission field
                file_stat->remotePerm.setPermission(RemotePermissions::IsShared);
            }
        }
    }
}

void DiscoverySingleDirectoryJob::directoryListingIteratedSlot(QString file, const QMap<QString, QString> &map)
{
    if (!_ignoredFirst) {
        // The first entry is for the folder itself, we should process it differently.
        _ignoredFirst = true;
        if (map.contains("permissions")) {
            RemotePermissions perm(map.value("permissions"));
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
    } else {
        // Remove <webDAV-Url>/folder/ from <webDAV-Url>/folder/subfile.txt
        file.remove(0, _lsColJob->reply()->request().url().path().length());
        // remove trailing slash
        while (file.endsWith('/')) {
            file.chop(1);
        }
        // remove leading slash
        while (file.startsWith('/')) {
            file = file.remove(0, 1);
        }

        std::unique_ptr<csync_file_stat_t> file_stat(new csync_file_stat_t);
        file_stat->path = file.toUtf8();
        file_stat->size = -1;
        propertyMapToFileStat(map, file_stat.get());
        if (file_stat->type == ItemTypeDirectory)
            file_stat->size = 0;
        if (file_stat->remotePerm.hasPermission(RemotePermissions::IsShared) && file_stat->etag.isEmpty()) {
            /* Handle broken shared file error gracefully instead of stopping sync in the desktop client.
               DO not set _error */
            qCWarning(lcDiscovery)
                << "Missing path to a share :" << file << file_stat->path << file_stat->type << file_stat->size
                << file_stat->modtime << file_stat->remotePerm.toString()
                << file_stat->etag << file_stat->file_id;
        } else if (file_stat->type == ItemTypeSkip
            || file_stat->size == -1
            || file_stat->remotePerm.isNull()
            || file_stat->etag.isEmpty()
            || file_stat->file_id.isEmpty()) {
            _error = tr("The server file discovery reply is missing data.");
            qCWarning(lcDiscovery)
                << "Missing properties:" << file << file_stat->type << file_stat->size
                << file_stat->modtime << file_stat->remotePerm.toString()
                << file_stat->etag << file_stat->file_id;
        }

        if (_isExternalStorage && file_stat->remotePerm.hasPermission(RemotePermissions::IsMounted)) {
            /* All the entries in a external storage have 'M' in their permission. However, for all
               purposes in the desktop client, we only need to know about the mount points.
               So replace the 'M' by a 'm' for every sub entries in an external storage */
            file_stat->remotePerm.unsetPermission(RemotePermissions::IsMounted);
            file_stat->remotePerm.setPermission(RemotePermissions::IsMountedSub);
        }

        QStringRef fileRef(&file);
        int slashPos = file.lastIndexOf(QLatin1Char('/'));
        if (slashPos > -1) {
            fileRef = file.midRef(slashPos + 1);
        }
        _results.push_back(std::move(file_stat));
    }

    //This works in concerto with the RequestEtagJob and the Folder object to check if the remote folder changed.
    if (map.contains("getetag")) {
        _etagConcatenation += map.value("getetag");

        if (_firstEtag.isEmpty()) {
            _firstEtag = map.value("getetag"); // for directory itself
        }
    }
}

void DiscoverySingleDirectoryJob::lsJobFinishedWithoutErrorSlot()
{
    if (!_ignoredFirst) {
        // This is a sanity check, if we haven't _ignoredFirst then it means we never received any directoryListingIteratedSlot
        // which means somehow the server XML was bogus
        emit finishedWithError(ERRNO_WRONG_CONTENT, QLatin1String("Server error: PROPFIND reply is not XML formatted!"));
        deleteLater();
        return;
    } else if (!_error.isEmpty()) {
        emit finishedWithError(ERRNO_WRONG_CONTENT, _error);
        deleteLater();
        return;
    }
    emit etag(_firstEtag);
    emit etagConcatenation(_etagConcatenation);
    emit finishedWithResult();
    deleteLater();
}

void DiscoverySingleDirectoryJob::lsJobFinishedWithErrorSlot(QNetworkReply *r)
{
    QString contentType = r->header(QNetworkRequest::ContentTypeHeader).toString();
    int httpCode = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString httpReason = r->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    QString msg = r->errorString();
    int errnoCode = EIO; // Something went wrong
    qCWarning(lcDiscovery) << "LSCOL job error" << r->errorString() << httpCode << r->error();
    if (httpCode != 0 && httpCode != 207) {
        errnoCode = get_errno_from_http_errcode(httpCode, httpReason);
    } else if (r->error() != QNetworkReply::NoError) {
        errnoCode = EIO;
    } else if (!contentType.contains("application/xml; charset=utf-8")) {
        msg = QLatin1String("Server error: PROPFIND reply is not XML formatted!");
        errnoCode = ERRNO_WRONG_CONTENT;
    } else {
        // Default keep at EIO, see above
    }

    emit finishedWithError(errnoCode == 0 ? EIO : errnoCode, msg);
    deleteLater();
}

void DiscoveryMainThread::setupHooks(DiscoveryJob *discoveryJob, const QString &pathPrefix)
{
    _discoveryJob = discoveryJob;
    _pathPrefix = pathPrefix;

    connect(discoveryJob, &DiscoveryJob::doOpendirSignal,
        this, &DiscoveryMainThread::doOpendirSlot,
        Qt::QueuedConnection);
    connect(discoveryJob, &DiscoveryJob::doGetSizeSignal,
        this, &DiscoveryMainThread::doGetSizeSlot,
        Qt::QueuedConnection);
}

// Coming from owncloud_opendir -> DiscoveryJob::vio_opendir_hook -> doOpendirSignal
void DiscoveryMainThread::doOpendirSlot(const QString &subPath, DiscoveryDirectoryResult *r)
{
    QString fullPath = _pathPrefix;
    if (!_pathPrefix.endsWith('/')) {
        fullPath += '/';
    }
    fullPath += subPath;
    // remove trailing slash
    while (fullPath.endsWith('/')) {
        fullPath.chop(1);
    }

    _discoveryJob->update_job_update_callback(/*local=*/false, subPath.toUtf8(), _discoveryJob);

    // Result gets written in there
    _currentDiscoveryDirectoryResult = r;
    _currentDiscoveryDirectoryResult->path = fullPath;

    // Schedule the DiscoverySingleDirectoryJob
    _singleDirJob = new DiscoverySingleDirectoryJob(_account, fullPath, this);
    QObject::connect(_singleDirJob.data(), &DiscoverySingleDirectoryJob::finishedWithResult,
        this, &DiscoveryMainThread::singleDirectoryJobResultSlot);
    QObject::connect(_singleDirJob.data(), &DiscoverySingleDirectoryJob::finishedWithError,
        this, &DiscoveryMainThread::singleDirectoryJobFinishedWithErrorSlot);
    QObject::connect(_singleDirJob.data(), &DiscoverySingleDirectoryJob::firstDirectoryPermissions,
        this, &DiscoveryMainThread::singleDirectoryJobFirstDirectoryPermissionsSlot);
    QObject::connect(_singleDirJob.data(), &DiscoverySingleDirectoryJob::etagConcatenation,
        this, &DiscoveryMainThread::etagConcatenation);
    QObject::connect(_singleDirJob.data(), &DiscoverySingleDirectoryJob::etag,
        this, &DiscoveryMainThread::etag);

    if (!_firstFolderProcessed) {
        _singleDirJob->setIsRootPath();
    }

    _singleDirJob->start();
}


void DiscoveryMainThread::singleDirectoryJobResultSlot()
{
    if (!_currentDiscoveryDirectoryResult) {
        return; // possibly aborted
    }

    _currentDiscoveryDirectoryResult->list = _singleDirJob->takeResults();
    _currentDiscoveryDirectoryResult->code = 0;

    qCDebug(lcDiscovery) << "Have" << _currentDiscoveryDirectoryResult->list.size() << "results for " << _currentDiscoveryDirectoryResult->path;

    _currentDiscoveryDirectoryResult = nullptr; // the sync thread owns it now

    if (!_firstFolderProcessed) {
        _firstFolderProcessed = true;
        _dataFingerprint = _singleDirJob->_dataFingerprint;
    }

    _discoveryJob->_vioMutex.lock();
    _discoveryJob->_vioWaitCondition.wakeAll();
    _discoveryJob->_vioMutex.unlock();
}

void DiscoveryMainThread::singleDirectoryJobFinishedWithErrorSlot(int csyncErrnoCode, const QString &msg)
{
    if (!_currentDiscoveryDirectoryResult) {
        return; // possibly aborted
    }
    qCDebug(lcDiscovery) << csyncErrnoCode << msg;

    _currentDiscoveryDirectoryResult->code = csyncErrnoCode;
    _currentDiscoveryDirectoryResult->msg = msg;
    _currentDiscoveryDirectoryResult = nullptr; // the sync thread owns it now

    _discoveryJob->_vioMutex.lock();
    _discoveryJob->_vioWaitCondition.wakeAll();
    _discoveryJob->_vioMutex.unlock();
}

void DiscoveryMainThread::singleDirectoryJobFirstDirectoryPermissionsSlot(RemotePermissions p)
{
    // Should be thread safe since the sync thread is blocked
    if (_discoveryJob->_csync_ctx->remote.root_perms.isNull()) {
        qCDebug(lcDiscovery) << "Permissions for root dir:" << p.toString();
        _discoveryJob->_csync_ctx->remote.root_perms = p;
    }
}

void DiscoveryMainThread::doGetSizeSlot(const QString &path, qint64 *result)
{
    QString fullPath = _pathPrefix;
    if (!_pathPrefix.endsWith('/')) {
        fullPath += '/';
    }
    fullPath += path;
    // remove trailing slash
    while (fullPath.endsWith('/')) {
        fullPath.chop(1);
    }

    _currentGetSizeResult = result;

    // Schedule the DiscoverySingleDirectoryJob
    auto propfindJob = new PropfindJob(_account, fullPath, this);
    propfindJob->setProperties(QList<QByteArray>() << "resourcetype"
                                                   << "http://owncloud.org/ns:size");
    QObject::connect(propfindJob, &PropfindJob::finishedWithError,
        this, &DiscoveryMainThread::slotGetSizeFinishedWithError);
    QObject::connect(propfindJob, &PropfindJob::result,
        this, &DiscoveryMainThread::slotGetSizeResult);
    propfindJob->start();
}

void DiscoveryMainThread::slotGetSizeFinishedWithError()
{
    if (!_currentGetSizeResult) {
        return; // possibly aborted
    }

    qCWarning(lcDiscovery) << "Error getting the size of the directory";
    // just let let the discovery job continue then
    _currentGetSizeResult = nullptr;
    QMutexLocker locker(&_discoveryJob->_vioMutex);
    _discoveryJob->_vioWaitCondition.wakeAll();
}

void DiscoveryMainThread::slotGetSizeResult(const QVariantMap &map)
{
    if (!_currentGetSizeResult) {
        return; // possibly aborted
    }

    *_currentGetSizeResult = map.value(QLatin1String("size")).toLongLong();
    qCDebug(lcDiscovery) << "Size of folder:" << *_currentGetSizeResult;
    _currentGetSizeResult = nullptr;
    QMutexLocker locker(&_discoveryJob->_vioMutex);
    _discoveryJob->_vioWaitCondition.wakeAll();
}


// called from SyncEngine
void DiscoveryMainThread::abort()
{
    if (_singleDirJob) {
        disconnect(_singleDirJob.data(), &DiscoverySingleDirectoryJob::finishedWithError, this, nullptr);
        disconnect(_singleDirJob.data(), &DiscoverySingleDirectoryJob::firstDirectoryPermissions, this, nullptr);
        disconnect(_singleDirJob.data(), &DiscoverySingleDirectoryJob::finishedWithResult, this, nullptr);
        _singleDirJob->abort();
    }
    if (_currentDiscoveryDirectoryResult) {
        if (_discoveryJob->_vioMutex.tryLock()) {
            _currentDiscoveryDirectoryResult->msg = tr("Aborted by the user"); // Actually also created somewhere else by sync engine
            _currentDiscoveryDirectoryResult->code = EIO;
            _currentDiscoveryDirectoryResult = nullptr;
            _discoveryJob->_vioWaitCondition.wakeAll();
            _discoveryJob->_vioMutex.unlock();
        }
    }
    if (_currentGetSizeResult) {
        _currentGetSizeResult = nullptr;
        QMutexLocker locker(&_discoveryJob->_vioMutex);
        _discoveryJob->_vioWaitCondition.wakeAll();
    }
}

csync_vio_handle_t *DiscoveryJob::remote_vio_opendir_hook(const char *url,
    void *userdata)
{
    DiscoveryJob *discoveryJob = static_cast<DiscoveryJob *>(userdata);
    if (discoveryJob) {
        qCDebug(lcDiscovery) << discoveryJob << url << "Calling into main thread...";

        QScopedPointer<DiscoveryDirectoryResult> directoryResult(new DiscoveryDirectoryResult());
        directoryResult->code = EIO;

        discoveryJob->_vioMutex.lock();
        const QString qurl = QString::fromUtf8(url);
        emit discoveryJob->doOpendirSignal(qurl, directoryResult.data());
        discoveryJob->_vioWaitCondition.wait(&discoveryJob->_vioMutex, ULONG_MAX); // FIXME timeout?
        discoveryJob->_vioMutex.unlock();

        qCDebug(lcDiscovery) << discoveryJob << url << "...Returned from main thread";

        // Upon awakening from the _vioWaitCondition, iterator should be a valid iterator.
        if (directoryResult->code != 0) {
            qCDebug(lcDiscovery) << directoryResult->code << "when opening" << url << "msg=" << directoryResult->msg;
            errno = directoryResult->code;
            // save the error string to the context
            discoveryJob->_csync_ctx->error_string = qstrdup(directoryResult->msg.toUtf8().constData());
            return nullptr;
        }

        return directoryResult.take();
    }
    return nullptr;
}


std::unique_ptr<csync_file_stat_t> DiscoveryJob::remote_vio_readdir_hook(csync_vio_handle_t *dhandle,
    void *userdata)
{
    DiscoveryJob *discoveryJob = static_cast<DiscoveryJob *>(userdata);
    if (discoveryJob) {
        DiscoveryDirectoryResult *directoryResult = static_cast<DiscoveryDirectoryResult *>(dhandle);
        if (!directoryResult->list.empty()) {
            auto file_stat = std::move(directoryResult->list.front());
            directoryResult->list.pop_front();
            return file_stat;
        }
    }
    return nullptr;
}

void DiscoveryJob::remote_vio_closedir_hook(csync_vio_handle_t *dhandle, void *userdata)
{
    DiscoveryJob *discoveryJob = static_cast<DiscoveryJob *>(userdata);
    if (discoveryJob) {
        DiscoveryDirectoryResult *directoryResult = static_cast<DiscoveryDirectoryResult *>(dhandle);
        QString path = directoryResult->path;
        qCDebug(lcDiscovery) << discoveryJob << path;
        // just deletes the struct and the iterator, the data itself is owned by the SyncEngine/DiscoveryMainThread
        delete directoryResult;
    }
}

void DiscoveryJob::start()
{
    _selectiveSyncBlackList.sort();
    _selectiveSyncWhiteList.sort();
    _csync_ctx->callbacks.update_callback_userdata = this;
    _csync_ctx->callbacks.update_callback = update_job_update_callback;
    _csync_ctx->callbacks.checkSelectiveSyncBlackListHook = isInSelectiveSyncBlackListCallback;
    _csync_ctx->callbacks.checkSelectiveSyncNewObjectHook = checkSelectiveSyncNewObjectCallback;

    _csync_ctx->callbacks.remote_opendir_hook = remote_vio_opendir_hook;
    _csync_ctx->callbacks.remote_readdir_hook = remote_vio_readdir_hook;
    _csync_ctx->callbacks.remote_closedir_hook = remote_vio_closedir_hook;
    _csync_ctx->callbacks.vio_userdata = this;

    _lastUpdateProgressCallbackCall.invalidate();
    int ret = csync_update(_csync_ctx);

    _csync_ctx->callbacks.checkSelectiveSyncNewObjectHook = nullptr;
    _csync_ctx->callbacks.checkSelectiveSyncBlackListHook = nullptr;
    _csync_ctx->callbacks.update_callback = nullptr;
    _csync_ctx->callbacks.update_callback_userdata = nullptr;

    emit finished(ret);
    deleteLater();
}
}
