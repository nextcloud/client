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
#pragma once

#include "owncloudlib.h"
#include "owncloudpropagator.h"
#include "networkjobs.h"
#include "clientsideencryption.h"

#include <QBuffer>
#include <QFile>

namespace OCC {
class PropagateDownloadEncrypted;

/**
 * @brief The GETFileJob class
 * @ingroup libsync
 */
class OWNCLOUDSYNC_EXPORT GETFileJob : public AbstractNetworkJob
{
    Q_OBJECT
    QMap<QByteArray, QByteArray> _headers;
    QString _errorString;
    QByteArray _expectedEtagForResume;
    qint64 _expectedContentLength;
    qint64 _resumeStart;
    SyncFileItem::Status _errorStatus;
    QUrl _directDownloadUrl;
    QByteArray _etag;
    bool _bandwidthLimited; // if _bandwidthQuota will be used
    bool _bandwidthChoked; // if download is paused (won't read on readyRead())
    qint64 _bandwidthQuota;
    QPointer<BandwidthManager> _bandwidthManager;
    bool _hasEmittedFinishedSignal;
    time_t _lastModified;

    /// Will be set to true once we've seen a 2xx response header
    bool _saveBodyToFile = false;

protected:
    QIODevice *_device;
    qint64 _contentLength;

public:
    // DOES NOT take ownership of the device.
    explicit GETFileJob(AccountPtr account, const QString &path, QIODevice *device,
        const QMap<QByteArray, QByteArray> &headers, const QByteArray &expectedEtagForResume,
        qint64 resumeStart, QObject *parent = nullptr);
    // For directDownloadUrl:
    explicit GETFileJob(AccountPtr account, const QUrl &url, QIODevice *device,
        const QMap<QByteArray, QByteArray> &headers, const QByteArray &expectedEtagForResume,
        qint64 resumeStart, QObject *parent = nullptr);
    virtual ~GETFileJob()
    {
        if (_bandwidthManager) {
            _bandwidthManager->unregisterDownloadJob(this);
        }
    }

    void start() override;
    bool finished() override
    {
        if (_saveBodyToFile && reply()->bytesAvailable()) {
            return false;
        } else {
            if (_bandwidthManager) {
                _bandwidthManager->unregisterDownloadJob(this);
            }
            if (!_hasEmittedFinishedSignal) {
                emit finishedSignal();
            }
            _hasEmittedFinishedSignal = true;
            return true; // discard
        }
    }

    void cancel();

    void newReplyHook(QNetworkReply *reply) override;

    void setBandwidthManager(BandwidthManager *bwm);
    void setChoked(bool c);
    void setBandwidthLimited(bool b);
    void giveBandwidthQuota(qint64 q);
    qint64 currentDownloadPosition();

    QString errorString() const;
    void setErrorString(const QString &s) { _errorString = s; }

    SyncFileItem::Status errorStatus() { return _errorStatus; }
    void setErrorStatus(const SyncFileItem::Status &s) { _errorStatus = s; }

    void onTimedOut() override;

    QByteArray &etag() { return _etag; }
    qint64 resumeStart() { return _resumeStart; }
    time_t lastModified() { return _lastModified; }

    qint64 contentLength() const { return _contentLength; }
    qint64 expectedContentLength() const { return _expectedContentLength; }
    void setExpectedContentLength(qint64 size) { _expectedContentLength = size; }

protected:
    virtual qint64 writeToDevice(const char *data, qint64 len);
    virtual void processMetaData() {}

signals:
    void finishedSignal();
    void downloadProgress(qint64, qint64);
private slots:
    void slotReadyRead();
    void slotMetaDataChanged();
};

/**
 * @brief The GETEncryptedFileJob class that provides file decryption on the fly while the download is running
 * @ingroup libsync
 */
class OWNCLOUDSYNC_EXPORT GETEncryptedFileJob : public GETFileJob
{
    Q_OBJECT

public:
    // DOES NOT take ownership of the device.
    explicit GETEncryptedFileJob(AccountPtr account, const QString &path, QIODevice *device,
        const QMap<QByteArray, QByteArray> &headers, const QByteArray &expectedEtagForResume,
        qint64 resumeStart, EncryptedFile encryptedInfo, QObject *parent = nullptr);
    explicit GETEncryptedFileJob(AccountPtr account, const QUrl &url, QIODevice *device,
        const QMap<QByteArray, QByteArray> &headers, const QByteArray &expectedEtagForResume,
        qint64 resumeStart, EncryptedFile encryptedInfo, QObject *parent = nullptr);
    virtual ~GETEncryptedFileJob() = default;

protected:
    virtual qint64 writeToDevice(const char *data, qint64 len) override;
    virtual void processMetaData() override;

private:
    QSharedPointer<EncryptionHelper::StreamingDecryptor> _decryptor;
    EncryptedFile _encryptedFileInfo = {};
};

/**
 * @brief The PropagateDownloadFile class
 * @ingroup libsync
 *
 * This is the flow:

\code{.unparsed}
  start()
    |
    | deleteExistingFolder() if enabled
    |
    +--> mtime and size identical?
    |    then compute the local checksum
    |                               done?-> conflictChecksumComputed()
    |                                              |
    |                         checksum differs?    |
    +-> startDownload() <--------------------------+
          |                                        |
          +-> run a GETFileJob                     | checksum identical?
                                                   |
      done?-> slotGetFinished()                    |
                |                                  |
                +-> validate checksum header       |
                                                   |
      done?-> transmissionChecksumValidated()      |
                |                                  |
                +-> compute the content checksum   |
                                                   |
      done?-> contentChecksumComputed()            |
                |                                  |
                +-> downloadFinished()             |
                       |                           |
    +------------------+                           |
    |                                              |
    +-> updateMetadata() <-------------------------+

\endcode
 */
class PropagateDownloadFile : public PropagateItemJob
{
    Q_OBJECT
public:
    PropagateDownloadFile(OwncloudPropagator *propagator, const SyncFileItemPtr &item)
        : PropagateItemJob(propagator, item)
        , _resumeStart(0)
        , _downloadProgress(0)
        , _deleteExisting(false)
    {
    }
    void start() override;
    qint64 committedDiskSpace() const override;

    // We think it might finish quickly because it is a small file.
    bool isLikelyFinishedQuickly() override { return _item->_size < propagator()->smallFileSize(); }

    /**
     * Whether an existing folder with the same name may be deleted before
     * the download.
     *
     * If it's a non-empty folder, it'll be renamed to a conflict-style name
     * to preserve any non-synced content that may be inside.
     *
     * Default: false.
     */
    void setDeleteExistingFolder(bool enabled);

private slots:
    /// Called when ComputeChecksum on the local file finishes,
    /// maybe the local and remote checksums are identical?
    void conflictChecksumComputed(const QByteArray &checksumType, const QByteArray &checksum);
    /// Called to start downloading the remote file
    void startDownload();
    /// Called when the GETFileJob finishes
    void slotGetFinished();
    /// Called when the download's checksum header was validated
    void transmissionChecksumValidated(const QByteArray &checksumType, const QByteArray &checksum);
    /// Called when the download's checksum computation is done
    void contentChecksumComputed(const QByteArray &checksumType, const QByteArray &checksum);
    void downloadFinished();
    /// Called when it's time to update the db metadata
    void updateMetadata(bool isConflict);

    void abort(PropagatorJob::AbortType abortType) override;
    void slotDownloadProgress(qint64, qint64);
    void slotChecksumFail(const QString &errMsg, const QByteArray &checksumType, const QByteArray &checksum, const QString &filePath);

private:
    void startAfterIsEncryptedIsChecked();
    void deleteExistingFolder();

    void startContentChecksumCompute(const QByteArray &checksumType, const QString &path);

    qint64 _resumeStart;
    qint64 _downloadProgress;
    QPointer<GETFileJob> _job;
    QFile _tmpFile;
    bool _deleteExisting;
    bool _isEncrypted = false;
    EncryptedFile _encryptedInfo;
    ConflictRecord _conflictRecord;

    QElapsedTimer _stopwatch;

    PropagateDownloadEncrypted *_downloadEncryptedHelper = nullptr;
};
}
