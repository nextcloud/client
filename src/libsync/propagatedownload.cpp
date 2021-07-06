/*
 * Copyright (C) by Olivier Goffart <ogoffart@owncloud.com>
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

#include "config.h"
#include "owncloudpropagator_p.h"
#include "propagatedownload.h"
#include "networkjobs.h"
#include "account.h"
#include "common/syncjournaldb.h"
#include "common/syncjournalfilerecord.h"
#include "common/utility.h"
#include "filesystem.h"
#include "propagatorjobs.h"
#include "common/checksums.h"
#include "common/asserts.h"
#include "clientsideencryptionjobs.h"
#include "propagatedownloadencrypted.h"
#include "common/vfs.h"

#include "configfile.h"

#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QFileInfo>
#include <QDir>
#include <cmath>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {
    constexpr quint16 numChecksumFailuresAllowed = 1;
    constexpr char *checksumFailureDbRecordPrefix = "ChecksumValidationFailed_";
}

namespace OCC {

Q_LOGGING_CATEGORY(lcGetJob, "nextcloud.sync.networkjob.get", QtInfoMsg)
Q_LOGGING_CATEGORY(lcPropagateDownload, "nextcloud.sync.propagator.download", QtInfoMsg)

// Always coming in with forward slashes.
// In csync_excluded_no_ctx we ignore all files with longer than 254 chars
// This function also adds a dot at the beginning of the filename to hide the file on OS X and Linux
QString OWNCLOUDSYNC_EXPORT createDownloadTmpFileName(const QString &previous)
{
    QString tmpFileName;
    QString tmpPath;
    int slashPos = previous.lastIndexOf('/');
    // work with both pathed filenames and only filenames
    if (slashPos == -1) {
        tmpFileName = previous;
        tmpPath = QString();
    } else {
        tmpFileName = previous.mid(slashPos + 1);
        tmpPath = previous.left(slashPos);
    }
    int overhead = 1 + 1 + 2 + 8; // slash dot dot-tilde ffffffff"
    int spaceForFileName = qMin(254, tmpFileName.length() + overhead) - overhead;
    if (tmpPath.length() > 0) {
        return tmpPath + '/' + '.' + tmpFileName.left(spaceForFileName) + ".~" + (QString::number(uint(qrand() % 0xFFFFFFFF), 16));
    } else {
        return '.' + tmpFileName.left(spaceForFileName) + ".~" + (QString::number(uint(qrand() % 0xFFFFFFFF), 16));
    }
}

// DOES NOT take ownership of the device.
GETFileJob::GETFileJob(AccountPtr account, const QString &path, QIODevice *device,
    const QMap<QByteArray, QByteArray> &headers, const QByteArray &expectedEtagForResume,
    qint64 resumeStart, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
    , _device(device)
    , _headers(headers)
    , _expectedEtagForResume(expectedEtagForResume)
    , _expectedContentLength(-1)
    , _contentLength(-1)
    , _resumeStart(resumeStart)
    , _errorStatus(SyncFileItem::NoStatus)
    , _bandwidthLimited(false)
    , _bandwidthChoked(false)
    , _bandwidthQuota(0)
    , _bandwidthManager(nullptr)
    , _hasEmittedFinishedSignal(false)
    , _lastModified()
{
}

GETFileJob::GETFileJob(AccountPtr account, const QUrl &url, QIODevice *device,
    const QMap<QByteArray, QByteArray> &headers, const QByteArray &expectedEtagForResume,
    qint64 resumeStart, QObject *parent)
    : AbstractNetworkJob(account, url.toEncoded(), parent)
    , _device(device)
    , _headers(headers)
    , _expectedEtagForResume(expectedEtagForResume)
    , _expectedContentLength(-1)
    , _contentLength(-1)
    , _resumeStart(resumeStart)
    , _errorStatus(SyncFileItem::NoStatus)
    , _directDownloadUrl(url)
    , _bandwidthLimited(false)
    , _bandwidthChoked(false)
    , _bandwidthQuota(0)
    , _bandwidthManager(nullptr)
    , _hasEmittedFinishedSignal(false)
    , _lastModified()
{
}


void GETFileJob::start()
{
    if (_resumeStart > 0) {
        _headers["Range"] = "bytes=" + QByteArray::number(_resumeStart) + '-';
        _headers["Accept-Ranges"] = "bytes";
        qCDebug(lcGetJob) << "Retry with range " << _headers["Range"];
    }

    QNetworkRequest req;
    for (QMap<QByteArray, QByteArray>::const_iterator it = _headers.begin(); it != _headers.end(); ++it) {
        req.setRawHeader(it.key(), it.value());
    }

    req.setPriority(QNetworkRequest::LowPriority); // Long downloads must not block non-propagation jobs.

    if (_directDownloadUrl.isEmpty()) {
        sendRequest("GET", makeDavUrl(path()), req);
    } else {
        // Use direct URL
        sendRequest("GET", _directDownloadUrl, req);
    }

    qCDebug(lcGetJob) << _bandwidthManager << _bandwidthChoked << _bandwidthLimited;
    if (_bandwidthManager) {
        _bandwidthManager->registerDownloadJob(this);
    }

    connect(this, &AbstractNetworkJob::networkActivity, account().data(), &Account::propagatorNetworkActivity);

    AbstractNetworkJob::start();
}

void GETFileJob::newReplyHook(QNetworkReply *reply)
{
    reply->setReadBufferSize(16 * 1024); // keep low so we can easier limit the bandwidth

    connect(reply, &QNetworkReply::metaDataChanged, this, &GETFileJob::slotMetaDataChanged);
    connect(reply, &QIODevice::readyRead, this, &GETFileJob::slotReadyRead);
    connect(reply, &QNetworkReply::finished, this, &GETFileJob::slotReadyRead);
    connect(reply, &QNetworkReply::downloadProgress, this, &GETFileJob::downloadProgress);
}

void GETFileJob::slotMetaDataChanged()
{
    // For some reason setting the read buffer in GETFileJob::start doesn't seem to go
    // through the HTTP layer thread(?)
    reply()->setReadBufferSize(16 * 1024);

    int httpStatus = reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (httpStatus == 301 || httpStatus == 302 || httpStatus == 303 || httpStatus == 307
        || httpStatus == 308 || httpStatus == 401) {
        // Redirects and auth failures (oauth token renew) are handled by AbstractNetworkJob and
        // will end up restarting the job. We do not want to process further data from the initial
        // request. newReplyHook() will reestablish signal connections for the follow-up request.
        bool ok = disconnect(reply(), &QNetworkReply::finished, this, &GETFileJob::slotReadyRead)
            && disconnect(reply(), &QNetworkReply::readyRead, this, &GETFileJob::slotReadyRead);
        ASSERT(ok);
        return;
    }

    // If the status code isn't 2xx, don't write the reply body to the file.
    // For any error: handle it when the job is finished, not here.
    if (httpStatus / 100 != 2) {
        // Disable the buffer limit, as we don't limit the bandwidth for error messages.
        // (We are only going to do a readAll() at the end.)
        reply()->setReadBufferSize(0);
        return;
    }
    if (reply()->error() != QNetworkReply::NoError) {
        return;
    }
    _etag = getEtagFromReply(reply());

    if (!_directDownloadUrl.isEmpty() && !_etag.isEmpty()) {
        qCInfo(lcGetJob) << "Direct download used, ignoring server ETag" << _etag;
        _etag = QByteArray(); // reset received ETag
    } else if (!_directDownloadUrl.isEmpty()) {
        // All fine, ETag empty and directDownloadUrl used
    } else if (_etag.isEmpty()) {
        qCWarning(lcGetJob) << "No E-Tag reply by server, considering it invalid";
        _errorString = tr("No E-Tag received from server, check Proxy/Gateway");
        _errorStatus = SyncFileItem::NormalError;
        reply()->abort();
        return;
    } else if (!_expectedEtagForResume.isEmpty() && _expectedEtagForResume != _etag) {
        qCWarning(lcGetJob) << "We received a different E-Tag for resuming!"
                            << _expectedEtagForResume << "vs" << _etag;
        _errorString = tr("We received a different E-Tag for resuming. Retrying next time.");
        _errorStatus = SyncFileItem::NormalError;
        reply()->abort();
        return;
    }

    bool ok = false;
    _contentLength = reply()->header(QNetworkRequest::ContentLengthHeader).toLongLong(&ok);
    if (ok && _expectedContentLength != -1 && _contentLength != _expectedContentLength) {
        qCWarning(lcGetJob) << "We received a different content length than expected!"
                            << _expectedContentLength << "vs" << _contentLength;
        _errorString = tr("We received an unexpected download Content-Length.");
        _errorStatus = SyncFileItem::NormalError;
        reply()->abort();
        return;
    }

    qint64 start = 0;
    QByteArray ranges = reply()->rawHeader("Content-Range");
    if (!ranges.isEmpty()) {
        QRegExp rx("bytes (\\d+)-");
        if (rx.indexIn(ranges) >= 0) {
            start = rx.cap(1).toLongLong();
        }
    }
    if (start != _resumeStart) {
        qCWarning(lcGetJob) << "Wrong content-range: " << ranges << " while expecting start was" << _resumeStart;
        if (ranges.isEmpty()) {
            // device doesn't support range, just try again from scratch
            _device->close();
            if (!_device->open(QIODevice::WriteOnly)) {
                _errorString = _device->errorString();
                _errorStatus = SyncFileItem::NormalError;
                reply()->abort();
                return;
            }
            _resumeStart = 0;
        } else {
            _errorString = tr("Server returned wrong content-range");
            _errorStatus = SyncFileItem::NormalError;
            reply()->abort();
            return;
        }
    }

    auto lastModified = reply()->header(QNetworkRequest::LastModifiedHeader);
    if (!lastModified.isNull()) {
        _lastModified = Utility::qDateTimeToTime_t(lastModified.toDateTime());
    }

    _saveBodyToFile = true;
}

void GETFileJob::setBandwidthManager(BandwidthManager *bwm)
{
    _bandwidthManager = bwm;
}

void GETFileJob::setChoked(bool c)
{
    _bandwidthChoked = c;
    QMetaObject::invokeMethod(this, "slotReadyRead", Qt::QueuedConnection);
}

void GETFileJob::setBandwidthLimited(bool b)
{
    _bandwidthLimited = b;
    QMetaObject::invokeMethod(this, "slotReadyRead", Qt::QueuedConnection);
}

void GETFileJob::giveBandwidthQuota(qint64 q)
{
    _bandwidthQuota = q;
    qCDebug(lcGetJob) << "Got" << q << "bytes";
    QMetaObject::invokeMethod(this, "slotReadyRead", Qt::QueuedConnection);
}

qint64 GETFileJob::currentDownloadPosition()
{
    if (_device && _device->pos() > 0 && _device->pos() > qint64(_resumeStart)) {
        return _device->pos();
    }
    return _resumeStart;
}

void GETFileJob::slotReadyRead()
{
    if (!reply())
        return;
    int bufferSize = qMin(1024 * 8ll, reply()->bytesAvailable());
    QByteArray buffer(bufferSize, Qt::Uninitialized);

    while (reply()->bytesAvailable() > 0 && _saveBodyToFile) {
        if (_bandwidthChoked) {
            qCWarning(lcGetJob) << "Download choked";
            break;
        }
        qint64 toRead = bufferSize;
        if (_bandwidthLimited) {
            toRead = qMin(qint64(bufferSize), _bandwidthQuota);
            if (toRead == 0) {
                qCWarning(lcGetJob) << "Out of quota";
                break;
            }
            _bandwidthQuota -= toRead;
        }

        qint64 r = reply()->read(buffer.data(), toRead);
        if (r < 0) {
            _errorString = networkReplyErrorString(*reply());
            _errorStatus = SyncFileItem::NormalError;
            qCWarning(lcGetJob) << "Error while reading from device: " << _errorString;
            reply()->abort();
            return;
        }

        qint64 w = _device->write(buffer.constData(), r);
        if (w != r) {
            _errorString = _device->errorString();
            _errorStatus = SyncFileItem::NormalError;
            qCWarning(lcGetJob) << "Error while writing to file" << w << r << _errorString;
            reply()->abort();
            return;
        }
    }

    if (reply()->isFinished() && (reply()->bytesAvailable() == 0 || !_saveBodyToFile)) {
        qCDebug(lcGetJob) << "Actually finished!";
        if (_bandwidthManager) {
            _bandwidthManager->unregisterDownloadJob(this);
        }
        if (!_hasEmittedFinishedSignal) {
            qCInfo(lcGetJob) << "GET of" << reply()->request().url().toString() << "FINISHED WITH STATUS"
                             << replyStatusString()
                             << reply()->rawHeader("Content-Range") << reply()->rawHeader("Content-Length");

            emit finishedSignal();
        }
        _hasEmittedFinishedSignal = true;
        deleteLater();
    }
}

void GETFileJob::cancel()
{
    const auto networkReply = reply();
    if (networkReply && networkReply->isRunning()) {
        networkReply->abort();
    }
    if (_device && _device->isOpen()) {
        _device->close();
    }
}

void GETFileJob::onTimedOut()
{
    qCWarning(lcGetJob) << "Timeout" << (reply() ? reply()->request().url() : path());
    if (!reply())
        return;
    _errorString = tr("Connection Timeout");
    _errorStatus = SyncFileItem::FatalError;
    reply()->abort();
}

QString GETFileJob::errorString() const
{
    if (!_errorString.isEmpty()) {
        return _errorString;
    }
    return AbstractNetworkJob::errorString();
}

void PropagateDownloadFile::start()
{
    if (propagator()->_abortRequested)
        return;
    _isEncrypted = false;

    qCDebug(lcPropagateDownload) << _item->_file << propagator()->_activeJobList.count();

    const auto path = _item->_file;
    const auto slashPosition = path.lastIndexOf('/');
    const auto parentPath = slashPosition >= 0 ? path.left(slashPosition) : QString();

    SyncJournalFileRecord parentRec;
    propagator()->_journal->getFileRecord(parentPath, &parentRec);

    const auto account = propagator()->account();
    if (!account->capabilities().clientSideEncryptionAvailable() ||
        !parentRec.isValid() ||
        !parentRec._isE2eEncrypted) {
        startAfterIsEncryptedIsChecked();
    } else {
        _downloadEncryptedHelper = new PropagateDownloadEncrypted(propagator(), parentPath, _item, this);
        connect(_downloadEncryptedHelper, &PropagateDownloadEncrypted::fileMetadataFound, [this] {
          _isEncrypted = true;
          startAfterIsEncryptedIsChecked();
        });
        connect(_downloadEncryptedHelper, &PropagateDownloadEncrypted::failed, [this] {
          done(SyncFileItem::NormalError,
               tr("File %1 cannot be downloaded because encryption information is missing.").arg(QDir::toNativeSeparators(_item->_file)));
        });
        _downloadEncryptedHelper->start();
    }
}

void PropagateDownloadFile::startAfterIsEncryptedIsChecked()
{
    _stopwatch.start();

    auto &syncOptions = propagator()->syncOptions();
    auto &vfs = syncOptions._vfs;

    // For virtual files just dehydrate or create the file and be done
    if (_item->_type == ItemTypeVirtualFileDehydration) {
        QString fsPath = propagator()->fullLocalPath(_item->_file);
        if (!FileSystem::verifyFileUnchanged(fsPath, _item->_previousSize, _item->_previousModtime)) {
            propagator()->_anotherSyncNeeded = true;
            done(SyncFileItem::SoftError, tr("File has changed since discovery"));
            return;
        }

        qCDebug(lcPropagateDownload) << "dehydrating file" << _item->_file;
        auto r = vfs->dehydratePlaceholder(*_item);
        if (!r) {
            done(SyncFileItem::NormalError, r.error());
            return;
        }
        propagator()->_journal->deleteFileRecord(_item->_originalFile);
        updateMetadata(false);

        if (!_item->_remotePerm.isNull() && !_item->_remotePerm.hasPermission(RemotePermissions::CanWrite)) {
            // make sure ReadOnly flag is preserved for placeholder, similarly to regular files
            FileSystem::setFileReadOnly(propagator()->fullLocalPath(_item->_file), true);
        }

        return;
    }
    if (vfs->mode() == Vfs::Off && _item->_type == ItemTypeVirtualFile) {
        qCWarning(lcPropagateDownload) << "ignored virtual file type of" << _item->_file;
        _item->_type = ItemTypeFile;
    }
    if (_item->_type == ItemTypeVirtualFile) {
        if (propagator()->localFileNameClash(_item->_file)) {
            done(SyncFileItem::NormalError, tr("File %1 cannot be downloaded because of a local file name clash!").arg(QDir::toNativeSeparators(_item->_file)));
            return;
        }

        qCDebug(lcPropagateDownload) << "creating virtual file" << _item->_file;
        auto r = vfs->createPlaceholder(*_item);
        if (!r) {
            done(SyncFileItem::NormalError, r.error());
            return;
        }
        updateMetadata(false);

        if (!_item->_remotePerm.isNull() && !_item->_remotePerm.hasPermission(RemotePermissions::CanWrite)) {
            // make sure ReadOnly flag is preserved for placeholder, similarly to regular files
            FileSystem::setFileReadOnly(propagator()->fullLocalPath(_item->_file), true);
        }

        return;
    }

    if (_deleteExisting) {
        deleteExistingFolder();

        // check for error with deletion
        if (_state == Finished) {
            return;
        }
    }

    // If we have a conflict where size of the file is unchanged,
    // compare the remote checksum to the local one.
    // Maybe it's not a real conflict and no download is necessary!
    // If the hashes are collision safe and identical, we assume the content is too.
    // For weak checksums, we only do that if the mtimes are also identical.

    const auto csync_is_collision_safe_hash = [](const QByteArray &checksum_header)
    {
        return checksum_header.startsWith("SHA")
            || checksum_header.startsWith("MD5:");
    };
    if (_item->_instruction == CSYNC_INSTRUCTION_CONFLICT
        && _item->_size == _item->_previousSize
        && !_item->_checksumHeader.isEmpty()
        && (csync_is_collision_safe_hash(_item->_checksumHeader)
            || _item->_modtime == _item->_previousModtime)) {
        qCDebug(lcPropagateDownload) << _item->_file << "may not need download, computing checksum";
        auto computeChecksum = new ComputeChecksum(this);
        computeChecksum->setChecksumType(parseChecksumHeaderType(_item->_checksumHeader));
        connect(computeChecksum, &ComputeChecksum::done,
            this, &PropagateDownloadFile::conflictChecksumComputed);
        propagator()->_activeJobList.append(this);
        computeChecksum->start(propagator()->fullLocalPath(_item->_file));
        return;
    }

    startDownload();
}

void PropagateDownloadFile::conflictChecksumComputed(const QByteArray &checksumType, const QByteArray &checksum)
{
    propagator()->_activeJobList.removeOne(this);
    if (makeChecksumHeader(checksumType, checksum) == _item->_checksumHeader) {
        // No download necessary, just update fs and journal metadata
        qCDebug(lcPropagateDownload) << _item->_file << "remote and local checksum match";

        // Apply the server mtime locally if necessary, ensuring the journal
        // and local mtimes end up identical
        auto fn = propagator()->fullLocalPath(_item->_file);
        if (_item->_modtime != _item->_previousModtime) {
            FileSystem::setModTime(fn, _item->_modtime);
            emit propagator()->touchedFile(fn);
        }
        _item->_modtime = FileSystem::getModTime(fn);
        updateMetadata(/*isConflict=*/false);
        return;
    }
    startDownload();
}

void PropagateDownloadFile::startDownload()
{
    if (propagator()->_abortRequested)
        return;

    // do a klaas' case clash check.
    if (propagator()->localFileNameClash(_item->_file)) {
        done(SyncFileItem::NormalError, tr("File %1 cannot be downloaded because of a local file name clash!").arg(QDir::toNativeSeparators(_item->_file)));
        return;
    }

    propagator()->reportProgress(*_item, 0);

    QString tmpFileName;
    QByteArray expectedEtagForResume;
    const SyncJournalDb::DownloadInfo progressInfo = propagator()->_journal->getDownloadInfo(_item->_file);
    if (progressInfo._valid) {
        // if the etag has changed meanwhile, remove the already downloaded part.
        if (progressInfo._etag != _item->_etag) {
            FileSystem::remove(propagator()->fullLocalPath(progressInfo._tmpfile));
            propagator()->_journal->setDownloadInfo(_item->_file, SyncJournalDb::DownloadInfo());
        } else {
            tmpFileName = progressInfo._tmpfile;
            expectedEtagForResume = progressInfo._etag;
        }
    }

    if (tmpFileName.isEmpty()) {
        tmpFileName = createDownloadTmpFileName(_item->_file);
    }
    _tmpFile.setFileName(propagator()->fullLocalPath(tmpFileName));

    _resumeStart = _tmpFile.size();
    if (_resumeStart > 0 && _resumeStart == _item->_size) {
        qCInfo(lcPropagateDownload) << "File is already complete, no need to download";
        downloadFinished();
        return;
    }

    // Can't open(Append) read-only files, make sure to make
    // file writable if it exists.
    if (_tmpFile.exists())
        FileSystem::setFileReadOnly(_tmpFile.fileName(), false);
    if (!_tmpFile.open(QIODevice::Append | QIODevice::Unbuffered)) {
        qCWarning(lcPropagateDownload) << "could not open temporary file" << _tmpFile.fileName();
        done(SyncFileItem::NormalError, _tmpFile.errorString());
        return;
    }
    // Hide temporary after creation
    FileSystem::setFileHidden(_tmpFile.fileName(), true);

    // If there's not enough space to fully download this file, stop.
    const auto diskSpaceResult = propagator()->diskSpaceCheck();
    if (diskSpaceResult != OwncloudPropagator::DiskSpaceOk) {
        if (diskSpaceResult == OwncloudPropagator::DiskSpaceFailure) {
            // Using DetailError here will make the error not pop up in the account
            // tab: instead we'll generate a general "disk space low" message and show
            // these detail errors only in the error view.
            done(SyncFileItem::DetailError,
                tr("The download would reduce free local disk space below the limit"));
            emit propagator()->insufficientLocalStorage();
        } else if (diskSpaceResult == OwncloudPropagator::DiskSpaceCritical) {
            done(SyncFileItem::FatalError,
                tr("Free space on disk is less than %1").arg(Utility::octetsToString(criticalFreeSpaceLimit())));
        }

        // Remove the temporary, if empty.
        if (_resumeStart == 0) {
            _tmpFile.remove();
        }

        return;
    }

    {
        SyncJournalDb::DownloadInfo pi;
        pi._etag = _item->_etag;
        pi._tmpfile = tmpFileName;
        pi._valid = true;
        propagator()->_journal->setDownloadInfo(_item->_file, pi);
        propagator()->_journal->commit("download file start");
    }

    QMap<QByteArray, QByteArray> headers;

    if (_item->_directDownloadUrl.isEmpty()) {
        // Normal job, download from oC instance
        _job = new GETFileJob(propagator()->account(),
            propagator()->fullRemotePath(_isEncrypted ? _item->_encryptedFileName : _item->_file),
            &_tmpFile, headers, expectedEtagForResume, _resumeStart, this);
    } else {
        // We were provided a direct URL, use that one
        qCInfo(lcPropagateDownload) << "directDownloadUrl given for " << _item->_file << _item->_directDownloadUrl;

        if (!_item->_directDownloadCookies.isEmpty()) {
            headers["Cookie"] = _item->_directDownloadCookies.toUtf8();
        }

        QUrl url = QUrl::fromUserInput(_item->_directDownloadUrl);
        _job = new GETFileJob(propagator()->account(),
            url,
            &_tmpFile, headers, expectedEtagForResume, _resumeStart, this);
    }
    _job->setBandwidthManager(&propagator()->_bandwidthManager);
    connect(_job.data(), &GETFileJob::finishedSignal, this, &PropagateDownloadFile::slotGetFinished);
    connect(_job.data(), &GETFileJob::downloadProgress, this, &PropagateDownloadFile::slotDownloadProgress);
    propagator()->_activeJobList.append(this);
    _job->start();
}

qint64 PropagateDownloadFile::committedDiskSpace() const
{
    if (_state == Running) {
        return qBound(0LL, _item->_size - _resumeStart - _downloadProgress, _item->_size);
    }
    return 0;
}

void PropagateDownloadFile::setDeleteExistingFolder(bool enabled)
{
    _deleteExisting = enabled;
}

const char owncloudCustomSoftErrorStringC[] = "owncloud-custom-soft-error-string";
void PropagateDownloadFile::slotGetFinished()
{
    propagator()->_activeJobList.removeOne(this);

    GETFileJob *job = _job;
    ASSERT(job);

    _item->_httpErrorCode = job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    _item->_requestId = job->requestId();

    QNetworkReply::NetworkError err = job->reply()->error();
    if (err != QNetworkReply::NoError) {
        // If we sent a 'Range' header and get 416 back, we want to retry
        // without the header.
        const bool badRangeHeader = job->resumeStart() > 0 && _item->_httpErrorCode == 416;
        if (badRangeHeader) {
            qCWarning(lcPropagateDownload) << "server replied 416 to our range request, trying again without";
            propagator()->_anotherSyncNeeded = true;
        }

        // Getting a 404 probably means that the file was deleted on the server.
        const bool fileNotFound = _item->_httpErrorCode == 404;
        if (fileNotFound) {
            qCWarning(lcPropagateDownload) << "server replied 404, assuming file was deleted";
        }

        // Getting a 423 means that the file is locked
        const bool fileLocked = _item->_httpErrorCode == 423;
        if (fileLocked) {
            qCWarning(lcPropagateDownload) << "server replied 423, file is Locked";
        }

        // Don't keep the temporary file if it is empty or we
        // used a bad range header or the file's not on the server anymore.
        if (_tmpFile.exists() && (_tmpFile.size() == 0 || badRangeHeader || fileNotFound)) {
            _tmpFile.close();
            FileSystem::remove(_tmpFile.fileName());
            propagator()->_journal->setDownloadInfo(_item->_file, SyncJournalDb::DownloadInfo());
        }

        if (!_item->_directDownloadUrl.isEmpty() && err != QNetworkReply::OperationCanceledError) {
            // If this was with a direct download, retry without direct download
            qCWarning(lcPropagateDownload) << "Direct download of" << _item->_directDownloadUrl << "failed. Retrying through owncloud.";
            _item->_directDownloadUrl.clear();
            start();
            return;
        }

        // This gives a custom QNAM (by the user of libowncloudsync) to abort() a QNetworkReply in its metaDataChanged() slot and
        // set a custom error string to make this a soft error. In contrast to the default hard error this won't bring down
        // the whole sync and allows for a custom error message.
        QNetworkReply *reply = job->reply();
        if (err == QNetworkReply::OperationCanceledError && reply->property(owncloudCustomSoftErrorStringC).isValid()) {
            job->setErrorString(reply->property(owncloudCustomSoftErrorStringC).toString());
            job->setErrorStatus(SyncFileItem::SoftError);
        } else if (badRangeHeader) {
            // Can't do this in classifyError() because 416 without a
            // Range header should result in NormalError.
            job->setErrorStatus(SyncFileItem::SoftError);
        } else if (fileNotFound) {
            job->setErrorString(tr("File was deleted from server"));
            job->setErrorStatus(SyncFileItem::SoftError);

            // As a precaution against bugs that cause our database and the
            // reality on the server to diverge, rediscover this folder on the
            // next sync run.
            propagator()->_journal->schedulePathForRemoteDiscovery(_item->_file);
        }

        QByteArray errorBody;
        QString errorString = _item->_httpErrorCode >= 400 ? job->errorStringParsingBody(&errorBody)
                                                           : job->errorString();
        SyncFileItem::Status status = job->errorStatus();
        if (status == SyncFileItem::NoStatus) {
            status = classifyError(err, _item->_httpErrorCode,
                &propagator()->_anotherSyncNeeded, errorBody);
        }

        done(status, errorString);
        return;
    }

    _item->_responseTimeStamp = job->responseTimestamp();

    if (!job->etag().isEmpty()) {
        // The etag will be empty if we used a direct download URL.
        // (If it was really empty by the server, the GETFileJob will have errored
        _item->_etag = parseEtag(job->etag());
    }
    if (job->lastModified()) {
        // It is possible that the file was modified on the server since we did the discovery phase
        // so make sure we have the up-to-date time
        _item->_modtime = job->lastModified();
    }

    _tmpFile.close();
    _tmpFile.flush();

    /* Check that the size of the GET reply matches the file size. There have been cases
     * reported that if a server breaks behind a proxy, the GET is still a 200 but is
     * truncated, as described here: https://github.com/owncloud/mirall/issues/2528
     */
    const QByteArray sizeHeader("Content-Length");
    qint64 bodySize = job->reply()->rawHeader(sizeHeader).toLongLong();
    bool hasSizeHeader = !job->reply()->rawHeader(sizeHeader).isEmpty();

    // Qt removes the content-length header for transparently decompressed HTTP1 replies
    // but not for HTTP2 or SPDY replies. For these it remains and contains the size
    // of the compressed data. See QTBUG-73364.
    const auto contentEncoding = job->reply()->rawHeader("content-encoding").toLower();
    if ((contentEncoding == "gzip" || contentEncoding == "deflate")
        && (job->reply()->attribute(QNetworkRequest::HTTP2WasUsedAttribute).toBool()
         || job->reply()->attribute(QNetworkRequest::SpdyWasUsedAttribute).toBool())) {
        bodySize = 0;
        hasSizeHeader = false;
    }

    if (hasSizeHeader && _tmpFile.size() > 0 && bodySize == 0) {
        // Strange bug with broken webserver or webfirewall https://github.com/owncloud/client/issues/3373#issuecomment-122672322
        // This happened when trying to resume a file. The Content-Range header was files, Content-Length was == 0
        qCDebug(lcPropagateDownload) << bodySize << _item->_size << _tmpFile.size() << job->resumeStart();
        FileSystem::remove(_tmpFile.fileName());
        done(SyncFileItem::SoftError, QLatin1String("Broken webserver returning empty content length for non-empty file on resume"));
        return;
    }

    if (bodySize > 0 && bodySize != _tmpFile.size() - job->resumeStart()) {
        qCDebug(lcPropagateDownload) << bodySize << _tmpFile.size() << job->resumeStart();
        propagator()->_anotherSyncNeeded = true;
        done(SyncFileItem::SoftError, tr("The file could not be downloaded completely."));
        return;
    }

    if (_tmpFile.size() == 0 && _item->_size > 0) {
        FileSystem::remove(_tmpFile.fileName());
        done(SyncFileItem::NormalError,
            tr("The downloaded file is empty, but the server said it should have been %1.")
                .arg(Utility::octetsToString(_item->_size)));
        return;
    }

    // Did the file come with conflict headers? If so, store them now!
    // If we download conflict files but the server doesn't send conflict
    // headers, the record will be established by SyncEngine::conflictRecordMaintenance.
    // (we can't reliably determine the file id of the base file here,
    // it might still be downloaded in a parallel job and not exist in
    // the database yet!)
    if (job->reply()->rawHeader("OC-Conflict") == "1") {
        _conflictRecord.path = _item->_file.toUtf8();
        _conflictRecord.initialBasePath = job->reply()->rawHeader("OC-ConflictInitialBasePath");
        _conflictRecord.baseFileId = job->reply()->rawHeader("OC-ConflictBaseFileId");
        _conflictRecord.baseEtag = job->reply()->rawHeader("OC-ConflictBaseEtag");

        auto mtimeHeader = job->reply()->rawHeader("OC-ConflictBaseMtime");
        if (!mtimeHeader.isEmpty())
            _conflictRecord.baseModtime = mtimeHeader.toLongLong();

        // We don't set it yet. That will only be done when the download finished
        // successfully, much further down. Here we just grab the headers because the
        // job will be deleted later.
    }

    // Do checksum validation for the download. If there is no checksum header, the validator
    // will also emit the validated() signal to continue the flow in slot transmissionChecksumValidated()
    // as this is (still) also correct.
    auto *validator = new ValidateChecksumHeader(this);
    connect(validator, &ValidateChecksumHeader::validated,
        this, &PropagateDownloadFile::transmissionChecksumValidated);
    connect(validator, &ValidateChecksumHeader::validationFailed,
        this, &PropagateDownloadFile::slotChecksumFail);
    auto checksumHeader = findBestChecksum(job->reply()->rawHeader(checkSumHeaderC));
    auto contentMd5Header = job->reply()->rawHeader(contentMd5HeaderC);
    if (checksumHeader.isEmpty() && !contentMd5Header.isEmpty())
        checksumHeader = "MD5:" + contentMd5Header;
    validator->start(_tmpFile.fileName(), checksumHeader);
}

void PropagateDownloadFile::slotChecksumFail(const QString &errMsg, const QByteArray &checksumType, const QByteArray &checksum, const QString &filePath)
{
    if (!checksumType.isEmpty() && !checksum.isEmpty() && !filePath.isEmpty()) {
        ConfigFile cfgFile;

        if (cfgFile.allowChecksumValidationFail()) {
            const auto key = QString(checksumFailureDbRecordPrefix + _item->_fileId);
            const QStringList mismatchEntryForFileSplitted = propagator()->_journal->keyValueStoreGet(key).toString().split(":", QString::SkipEmptyParts);
            const QByteArray mismatchChecksumForFile = mismatchEntryForFileSplitted.size() > 0 ? mismatchEntryForFileSplitted[0].toUtf8() : QByteArray();
            const auto numChecksumMismatchCases = mismatchEntryForFileSplitted.size() > 1 ? mismatchEntryForFileSplitted[1].toInt() : 0;

            // format must be CHECKSUM:COUNT
            Q_ASSERT(mismatchEntryForFileSplitted.size() != 1);
            if (mismatchEntryForFileSplitted.size() == 1) {
                qCCritical(lcPropagateDownload) << "mismatchEntryForFile has incorrect format. Should be CHECKSUM:COUNT";
            }

            if (numChecksumMismatchCases < numChecksumFailuresAllowed || mismatchChecksumForFile != checksum) {
                // not enough failures or different checksum this time
                qCInfo(lcPropagateDownload) << "Checksum validation has failed" << numChecksumMismatchCases << " times, with previous checksum<" << mismatchChecksumForFile << "> and, current checksum<" << checksum << ">, but, allowChecksumValidationFail is set.Let's give it another try...";
                const auto numCasesToSet = mismatchChecksumForFile != checksum ? 1 : numChecksumMismatchCases + 1;
                const QString value = QString::fromUtf8(checksum) + QStringLiteral(":") + QString::number(numCasesToSet);
                propagator()->_journal->keyValueStoreSet(key, value);
            } else {
                propagator()->_journal->keyValueStoreDelete(key);
                qCInfo(lcPropagateDownload) << "Checksum validation has failed" << numChecksumMismatchCases << " times, but, allowChecksumValidationFail is set, so, let's continue...";
                startContentChecksumCompute(checksumType, filePath);
                return;
            }
        }
    }
    
    FileSystem::remove(_tmpFile.fileName());
    propagator()->_anotherSyncNeeded = true;
    done(SyncFileItem::SoftError, errMsg); // tr("The file downloaded with a broken checksum, will be redownloaded."));
}

void PropagateDownloadFile::deleteExistingFolder()
{
    QString existingDir = propagator()->fullLocalPath(_item->_file);
    if (!QFileInfo(existingDir).isDir()) {
        return;
    }

    // Delete the directory if it is empty!
    QDir dir(existingDir);
    if (dir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).count() == 0) {
        if (dir.rmdir(existingDir)) {
            return;
        }
        // on error, just try to move it away...
    }

    QString error;
    if (!propagator()->createConflict(_item, _associatedComposite, &error)) {
        done(SyncFileItem::NormalError, error);
    }
}

void PropagateDownloadFile::startContentChecksumCompute(const QByteArray &checksumType, const QString &path)
{
    qCInfo(lcPropagateDownload) << "Start checksum compute with checksumType:" << checksumType << " for path:" << path;
    // Compute the content checksum.
    const auto computeChecksum = new ComputeChecksum(this);
    computeChecksum->setChecksumType(checksumType);

    connect(computeChecksum, &ComputeChecksum::done,
        this, &PropagateDownloadFile::contentChecksumComputed);
    computeChecksum->start(path);
}

namespace { // Anonymous namespace for the recall feature
    static QString makeRecallFileName(const QString &fn)
    {
        QString recallFileName(fn);
        // Add _recall-XXXX  before the extension.
        int dotLocation = recallFileName.lastIndexOf('.');
        // If no extension, add it at the end  (take care of cases like foo/.hidden or foo.bar/file)
        if (dotLocation <= recallFileName.lastIndexOf('/') + 1) {
            dotLocation = recallFileName.size();
        }

        QString timeString = QDateTime::currentDateTimeUtc().toString("yyyyMMdd-hhmmss");
        recallFileName.insert(dotLocation, "_.sys.admin#recall#-" + timeString);

        return recallFileName;
    }

    void handleRecallFile(const QString &filePath, const QString &folderPath, SyncJournalDb &journal)
    {
        qCDebug(lcPropagateDownload) << "handleRecallFile: " << filePath;

        FileSystem::setFileHidden(filePath, true);

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly)) {
            qCWarning(lcPropagateDownload) << "Could not open recall file" << file.errorString();
            return;
        }
        QFileInfo existingFile(filePath);
        QDir baseDir = existingFile.dir();

        while (!file.atEnd()) {
            QByteArray line = file.readLine();
            line.chop(1); // remove trailing \n

            QString recalledFile = QDir::cleanPath(baseDir.filePath(line));
            if (!recalledFile.startsWith(folderPath) || !recalledFile.startsWith(baseDir.path())) {
                qCWarning(lcPropagateDownload) << "Ignoring recall of " << recalledFile;
                continue;
            }

            // Path of the recalled file in the local folder
            QString localRecalledFile = recalledFile.mid(folderPath.size());

            SyncJournalFileRecord record;
            if (!journal.getFileRecord(localRecalledFile, &record) || !record.isValid()) {
                qCWarning(lcPropagateDownload) << "No db entry for recall of" << localRecalledFile;
                continue;
            }

            qCInfo(lcPropagateDownload) << "Recalling" << localRecalledFile << "Checksum:" << record._checksumHeader;

            QString targetPath = makeRecallFileName(recalledFile);

            qCDebug(lcPropagateDownload) << "Copy recall file: " << recalledFile << " -> " << targetPath;
            // Remove the target first, QFile::copy will not overwrite it.
            FileSystem::remove(targetPath);
            QFile::copy(recalledFile, targetPath);
        }
    }

    static void preserveGroupOwnership(const QString &fileName, const QFileInfo &fi)
    {
#ifdef Q_OS_UNIX
        int chownErr = chown(fileName.toLocal8Bit().constData(), -1, fi.groupId());
        if (chownErr) {
            // TODO: Consider further error handling!
            qCWarning(lcPropagateDownload) << QString("preserveGroupOwnership: chown error %1: setting group %2 failed on file %3").arg(chownErr).arg(fi.groupId()).arg(fileName);
        }
#else
        Q_UNUSED(fileName);
        Q_UNUSED(fi);
#endif
    }
} // end namespace

void PropagateDownloadFile::transmissionChecksumValidated(const QByteArray &checksumType, const QByteArray &checksum)
{
    const QByteArray theContentChecksumType = propagator()->account()->capabilities().preferredUploadChecksumType();

    // Reuse transmission checksum as content checksum.
    //
    // We could do this more aggressively and accept both MD5 and SHA1
    // instead of insisting on the exactly correct checksum type.
    if (theContentChecksumType == checksumType || theContentChecksumType.isEmpty()) {
        return contentChecksumComputed(checksumType, checksum);
    }

    startContentChecksumCompute(theContentChecksumType, _tmpFile.fileName());

    propagator()->_journal->keyValueStoreDelete(QString(checksumFailureDbRecordPrefix + _item->_fileId));
}

void PropagateDownloadFile::contentChecksumComputed(const QByteArray &checksumType, const QByteArray &checksum)
{
    _item->_checksumHeader = makeChecksumHeader(checksumType, checksum);

    if (_isEncrypted) {
        if (_downloadEncryptedHelper->decryptFile(_tmpFile)) {
          downloadFinished();
        } else {
          done(SyncFileItem::NormalError, _downloadEncryptedHelper->errorString());
        }

    } else {
        downloadFinished();
    }
}

void PropagateDownloadFile::downloadFinished()
{
    ASSERT(!_tmpFile.isOpen());
    QString fn = propagator()->fullLocalPath(_item->_file);

    // In case of file name clash, report an error
    // This can happen if another parallel download saved a clashing file.
    if (propagator()->localFileNameClash(_item->_file)) {
        done(SyncFileItem::NormalError, tr("File %1 cannot be saved because of a local file name clash!").arg(QDir::toNativeSeparators(_item->_file)));
        return;
    }

    FileSystem::setModTime(_tmpFile.fileName(), _item->_modtime);
    // We need to fetch the time again because some file systems such as FAT have worse than a second
    // Accuracy, and we really need the time from the file system. (#3103)
    _item->_modtime = FileSystem::getModTime(_tmpFile.fileName());

    bool previousFileExists = FileSystem::fileExists(fn);
    if (previousFileExists) {
        // Preserve the existing file permissions.
        QFileInfo existingFile(fn);
        if (existingFile.permissions() != _tmpFile.permissions()) {
            _tmpFile.setPermissions(existingFile.permissions());
        }
        preserveGroupOwnership(_tmpFile.fileName(), existingFile);

        // Make the file a hydrated placeholder if possible
        const auto result = propagator()->syncOptions()._vfs->convertToPlaceholder(_tmpFile.fileName(), *_item, fn);
        if (!result) {
            done(SyncFileItem::NormalError, result.error());
            return;
        }
    }

    // Apply the remote permissions
    FileSystem::setFileReadOnlyWeak(_tmpFile.fileName(), !_item->_remotePerm.isNull() && !_item->_remotePerm.hasPermission(RemotePermissions::CanWrite));

    bool isConflict = _item->_instruction == CSYNC_INSTRUCTION_CONFLICT
        && (QFileInfo(fn).isDir() || !FileSystem::fileEquals(fn, _tmpFile.fileName()));
    if (isConflict) {
        QString error;
        if (!propagator()->createConflict(_item, _associatedComposite, &error)) {
            done(SyncFileItem::SoftError, error);
            return;
        }
        previousFileExists = false;
    }

    const auto vfs = propagator()->syncOptions()._vfs;

    // In the case of an hydration, this size is likely to change for placeholders
    // (except with the cfapi backend)
    const auto isVirtualDownload = _item->_type == ItemTypeVirtualFileDownload;
    const auto isCfApiVfs = vfs && vfs->mode() == Vfs::WindowsCfApi;
    if (previousFileExists && (isCfApiVfs || !isVirtualDownload)) {
        // Check whether the existing file has changed since the discovery
        // phase by comparing size and mtime to the previous values. This
        // is necessary to avoid overwriting user changes that happened between
        // the discovery phase and now.
        const qint64 expectedSize = _item->_previousSize;
        const time_t expectedMtime = _item->_previousModtime;
        if (!FileSystem::verifyFileUnchanged(fn, expectedSize, expectedMtime)) {
            propagator()->_anotherSyncNeeded = true;
            done(SyncFileItem::SoftError, tr("File has changed since discovery"));
            return;
        }
    }

    QString error;
    emit propagator()->touchedFile(fn);
    // The fileChanged() check is done above to generate better error messages.
    if (!FileSystem::uncheckedRenameReplace(_tmpFile.fileName(), fn, &error)) {
        qCWarning(lcPropagateDownload) << QString("Rename failed: %1 => %2").arg(_tmpFile.fileName()).arg(fn);
        // If the file is locked, we want to retry this sync when it
        // becomes available again, otherwise try again directly
        if (FileSystem::isFileLocked(fn)) {
            emit propagator()->seenLockedFile(fn);
        } else {
            propagator()->_anotherSyncNeeded = true;
        }

        done(SyncFileItem::SoftError, error);
        return;
    }

    FileSystem::setFileHidden(fn, false);

    // Maybe we downloaded a newer version of the file than we thought we would...
    // Get up to date information for the journal.
    _item->_size = FileSystem::getSize(fn);

    // Maybe what we downloaded was a conflict file? If so, set a conflict record.
    // (the data was prepared in slotGetFinished above)
    if (_conflictRecord.isValid())
        propagator()->_journal->setConflictRecord(_conflictRecord);

    if (vfs && vfs->mode() == Vfs::WithSuffix) {
        // If the virtual file used to have a different name and db
        // entry, remove it transfer its old pin state.
        if (_item->_type == ItemTypeVirtualFileDownload) {
            QString virtualFile = _item->_file + vfs->fileSuffix();
            auto fn = propagator()->fullLocalPath(virtualFile);
            qCDebug(lcPropagateDownload) << "Download of previous virtual file finished" << fn;
            QFile::remove(fn);
            propagator()->_journal->deleteFileRecord(virtualFile);

            // Move the pin state to the new location
            auto pin = propagator()->_journal->internalPinStates().rawForPath(virtualFile.toUtf8());
            if (pin && *pin != PinState::Inherited) {
                vfs->setPinState(_item->_file, *pin);
                vfs->setPinState(virtualFile, PinState::Inherited);
            }
        }

        // Ensure the pin state isn't contradictory
        auto pin = vfs->pinState(_item->_file);
        if (pin && *pin == PinState::OnlineOnly)
            vfs->setPinState(_item->_file, PinState::Unspecified);
    }

    updateMetadata(isConflict);
}

void PropagateDownloadFile::updateMetadata(bool isConflict)
{
    const QString fn = propagator()->fullLocalPath(_item->_file);
    const auto result = propagator()->updateMetadata(*_item);
    if (!result) {
        done(SyncFileItem::FatalError, tr("Error updating metadata: %1").arg(result.error()));
        return;
    } else if (*result == Vfs::ConvertToPlaceholderResult::Locked) {
        done(SyncFileItem::SoftError, tr("The file %1 is currently in use").arg(_item->_file));
        return;
    }

    if (_isEncrypted) {
        propagator()->_journal->setDownloadInfo(_item->_file, SyncJournalDb::DownloadInfo());
    } else {
        propagator()->_journal->setDownloadInfo(_item->_encryptedFileName, SyncJournalDb::DownloadInfo());
    }

    propagator()->_journal->commit("download file start2");

    done(isConflict ? SyncFileItem::Conflict : SyncFileItem::Success);

    // handle the special recall file
    if (!_item->_remotePerm.hasPermission(RemotePermissions::IsShared)
        && (_item->_file == QLatin1String(".sys.admin#recall#")
               || _item->_file.endsWith(QLatin1String("/.sys.admin#recall#")))) {
        handleRecallFile(fn, propagator()->localPath(), *propagator()->_journal);
    }

    qint64 duration = _stopwatch.elapsed();
    if (isLikelyFinishedQuickly() && duration > 5 * 1000) {
        qCWarning(lcPropagateDownload) << "WARNING: Unexpectedly slow connection, took" << duration << "msec for" << _item->_size - _resumeStart << "bytes for" << _item->_file;
    }
}

void PropagateDownloadFile::slotDownloadProgress(qint64 received, qint64)
{
    if (!_job)
        return;
    _downloadProgress = received;
    propagator()->reportProgress(*_item, _resumeStart + received);
}


void PropagateDownloadFile::abort(PropagatorJob::AbortType abortType)
{
    if (_job && _job->reply())
        _job->reply()->abort();

    if (abortType == AbortType::Asynchronous) {
        emit abortFinished();
    }
}
}
