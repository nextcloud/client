/*
 * Copyright (C) by Kevin Ottens <kevin.ottens@nextcloud.com>
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

#include "hydrationjob.h"

#include "common/syncjournaldb.h"
#include "propagatedownload.h"
#include "vfs/cfapi/vfs_cfapi.h"

#include <QLocalServer>
#include <QLocalSocket>

Q_LOGGING_CATEGORY(lcHydration, "nextcloud.sync.vfs.hydrationjob", QtInfoMsg)

OCC::HydrationJob::HydrationJob(QObject *parent)
    : QObject(parent)
{
}

OCC::AccountPtr OCC::HydrationJob::account() const
{
    return _account;
}

void OCC::HydrationJob::setAccount(const AccountPtr &account)
{
    _account = account;
}

QString OCC::HydrationJob::remotePath() const
{
    return _remotePath;
}

void OCC::HydrationJob::setRemotePath(const QString &remotePath)
{
    _remotePath = remotePath;
}

QString OCC::HydrationJob::localPath() const
{
    return _localPath;
}

void OCC::HydrationJob::setLocalPath(const QString &localPath)
{
    _localPath = localPath;
}

OCC::SyncJournalDb *OCC::HydrationJob::journal() const
{
    return _journal;
}

void OCC::HydrationJob::setJournal(SyncJournalDb *journal)
{
    _journal = journal;
}

QString OCC::HydrationJob::requestId() const
{
    return _requestId;
}

void OCC::HydrationJob::setRequestId(const QString &requestId)
{
    _requestId = requestId;
}

QString OCC::HydrationJob::folderPath() const
{
    return _folderPath;
}

void OCC::HydrationJob::setFolderPath(const QString &folderPath)
{
    _folderPath = folderPath;
}

OCC::HydrationJob::Status OCC::HydrationJob::status() const
{
    return _status;
}

void OCC::HydrationJob::start()
{
    Q_ASSERT(_account);
    Q_ASSERT(_journal);
    Q_ASSERT(!_remotePath.isEmpty() && !_localPath.isEmpty());
    Q_ASSERT(!_requestId.isEmpty() && !_folderPath.isEmpty());

    Q_ASSERT(_remotePath.endsWith('/'));
    Q_ASSERT(_localPath.endsWith('/'));
    Q_ASSERT(!_folderPath.startsWith('/'));

    const auto startServer = [this](const QString &serverName) -> QLocalServer * {
        const auto server = new QLocalServer(this);
        const auto listenResult = server->listen(serverName);
        if (!listenResult) {
            qCCritical(lcHydration) << "Couldn't get server to listen" << serverName
                                    << _localPath << _folderPath;
            if (!_isCancelled) {
                emitFinished(Error);
            }
            return nullptr;
        }
        qCInfo(lcHydration) << "Server ready, waiting for connections" << serverName
                            << _localPath << _folderPath;
        return server;
    };

    // Start cancellation server
    _signalServer = startServer(_requestId + ":cancellation");
    Q_ASSERT(_signalServer);
    if (!_signalServer) {
        return;
    }
    connect(_signalServer, &QLocalServer::newConnection, this, &HydrationJob::onCancellationServerNewConnection);

    // Start transfer data server
    _transferDataServer = startServer(_requestId);
    Q_ASSERT(_transferDataServer);
    if (!_transferDataServer) {
        return;
    }
    connect(_transferDataServer, &QLocalServer::newConnection, this, &HydrationJob::onNewConnection);
}

void OCC::HydrationJob::cancel()
{
    Q_ASSERT(_signalSocket);

    _isCancelled = true;
    if (_job) {
        _job->cancel();
    }

    _signalSocket->write("cancelled");
    _signalSocket->close();
    _transferDataSocket->close();

    emitFinished(Cancelled);
}

void OCC::HydrationJob::emitFinished(Status status)
{
    _status = status;
    _signalSocket->close();

    if (status == Success) {
        connect(_transferDataSocket, &QLocalSocket::disconnected, this, [=] {
            _transferDataSocket->close();
            emit finished(this);
        });
        _transferDataSocket->disconnectFromServer();
        return;
    }
    _transferDataSocket->close();

    emit finished(this);
}

void OCC::HydrationJob::onCancellationServerNewConnection()
{
    Q_ASSERT(!_signalSocket);

    qCInfo(lcHydration) << "Got new connection on cancellation server" << _requestId << _folderPath;
    _signalSocket = _signalServer->nextPendingConnection();
}

void OCC::HydrationJob::onNewConnection()
{
    Q_ASSERT(!_transferDataSocket);
    Q_ASSERT(!_job);

    qCInfo(lcHydration) << "Got new connection starting GETFileJob" << _requestId << _folderPath;
    _transferDataSocket = _transferDataServer->nextPendingConnection();
    _job = new GETFileJob(_account, _remotePath + _folderPath, _transferDataSocket, {}, {}, 0, this);
    connect(_job, &GETFileJob::finishedSignal, this, &HydrationJob::onGetFinished);
    _job->start();
}

void OCC::HydrationJob::finalize(OCC::VfsCfApi *vfs)
{
    // Mark the file as hydrated in the sync journal
    SyncJournalFileRecord record;
    _journal->getFileRecord(_folderPath, &record);
    Q_ASSERT(record.isValid());
    if (!record.isValid()) {
        qCWarning(lcHydration) << "Couldn't find record to update after hydration" << _requestId << _folderPath;
        // emitFinished(Error);
        return;
    }

    if (_isCancelled) {
        // Remove placeholder file because there might be already pumped
        // some data into it
        QFile::remove(_localPath + _folderPath);
        // Create a new placeholder file
        const auto item = SyncFileItem::fromSyncJournalFileRecord(record);
        vfs->createPlaceholder(*item);
        return;
    }

    record._type = ItemTypeFile;
    _journal->setFileRecord(record);
}

void OCC::HydrationJob::onGetFinished()
{
    qCInfo(lcHydration) << "GETFileJob finished" << _requestId << _folderPath << _job->reply()->error();

    const auto isGetJobResultError = _job->reply()->error();
    // GETFileJob deletes itself after this signal was handled
    _job = nullptr;
    if (_isCancelled) {
        return;
    }

    if (isGetJobResultError) {
        emitFinished(Error);
        return;
    }
    emitFinished(Success);
}
