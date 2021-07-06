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

#include "propagateremotemkdir.h"
#include "owncloudpropagator_p.h"
#include "account.h"
#include "common/syncjournalfilerecord.h"
#include "propagateuploadencrypted.h"
#include "deletejob.h"
#include "common/asserts.h"
#include "encryptfolderjob.h"

#include <QFile>
#include <QLoggingCategory>

namespace OCC {

Q_LOGGING_CATEGORY(lcPropagateRemoteMkdir, "nextcloud.sync.propagator.remotemkdir", QtInfoMsg)

PropagateRemoteMkdir::PropagateRemoteMkdir(OwncloudPropagator *propagator, const SyncFileItemPtr &item)
    : PropagateItemJob(propagator, item)
    , _deleteExisting(false)
    , _uploadEncryptedHelper(nullptr)
{
    const auto path = _item->_file;
    const auto slashPosition = path.lastIndexOf('/');
    const auto parentPath = slashPosition >= 0 ? path.left(slashPosition) : QString();

    SyncJournalFileRecord parentRec;
    bool ok = propagator->_journal->getFileRecord(parentPath, &parentRec);
    if (!ok) {
        return;
    }
}

void PropagateRemoteMkdir::start()
{
    if (propagator()->_abortRequested)
        return;

    qCDebug(lcPropagateRemoteMkdir) << _item->_file;

    propagator()->_activeJobList.append(this);

    if (!_deleteExisting) {
        slotMkdir();
        return;
    }

    _job = new DeleteJob(propagator()->account(),
        propagator()->fullRemotePath(_item->_file),
        this);
    connect(static_cast<DeleteJob*>(_job.data()), &DeleteJob::finishedSignal,
            this, &PropagateRemoteMkdir::slotMkdir);
    _job->start();
}

void PropagateRemoteMkdir::slotStartMkcolJob()
{
    if (propagator()->_abortRequested)
        return;

    qCDebug(lcPropagateRemoteMkdir) << _item->_file;

    _job = new MkColJob(propagator()->account(),
        propagator()->fullRemotePath(_item->_file),
        this);
    connect(_job, SIGNAL(finished(QNetworkReply::NetworkError)), this, SLOT(slotMkcolJobFinished()));
    _job->start();
}

void PropagateRemoteMkdir::slotStartEncryptedMkcolJob(const QString &path, const QString &filename, quint64 size)
{
    Q_UNUSED(path)
    Q_UNUSED(size)

    if (propagator()->_abortRequested)
        return;

    qDebug() << filename;
    qCDebug(lcPropagateRemoteMkdir) << filename;

    auto job = new MkColJob(propagator()->account(),
                            propagator()->fullRemotePath(filename),
                            {{"e2e-token", _uploadEncryptedHelper->folderToken() }},
                            this);
    connect(job, qOverload<QNetworkReply::NetworkError>(&MkColJob::finished),
            this, &PropagateRemoteMkdir::slotMkcolJobFinished);
    _job = job;
    _job->start();
}

void PropagateRemoteMkdir::abort(PropagatorJob::AbortType abortType)
{
    if (_job && _job->reply())
        _job->reply()->abort();

    if (abortType == AbortType::Asynchronous) {
        emit abortFinished();
    }
}

void PropagateRemoteMkdir::setDeleteExisting(bool enabled)
{
    _deleteExisting = enabled;
}

void PropagateRemoteMkdir::finalizeMkColJob(QNetworkReply::NetworkError err, const QString &jobHttpReasonPhraseString, const QString &jobPath)
{
    if (_item->_httpErrorCode == 405) {
        // This happens when the directory already exists. Nothing to do.
        qDebug(lcPropagateRemoteMkdir) << "Folder" << jobPath << "already exists.";
    } else if (err != QNetworkReply::NoError) {
        SyncFileItem::Status status = classifyError(err, _item->_httpErrorCode,
            &propagator()->_anotherSyncNeeded);
        done(status, _item->_errorString);
        return;
    } else if (_item->_httpErrorCode != 201) {
        // Normally we expect "201 Created"
        // If it is not the case, it might be because of a proxy or gateway intercepting the request, so we must
        // throw an error.
        done(SyncFileItem::NormalError,
            tr("Wrong HTTP code returned by server. Expected 201, but received \"%1 %2\".")
                .arg(_item->_httpErrorCode)
                .arg(jobHttpReasonPhraseString));
        return;
    }

    if (_item->_fileId.isEmpty()) {
        // Owncloud 7.0.0 and before did not have a header with the file id.
        // (https://github.com/owncloud/core/issues/9000)
        // So we must get the file id using a PROPFIND
        // This is required so that we can detect moves even if the folder is renamed on the server
        // while files are still uploading
        propagator()->_activeJobList.append(this);
        auto propfindJob = new PropfindJob(propagator()->account(), jobPath, this);
        propfindJob->setProperties(QList<QByteArray>() << "http://owncloud.org/ns:id");
        QObject::connect(propfindJob, &PropfindJob::result, this, &PropagateRemoteMkdir::propfindResult);
        QObject::connect(propfindJob, &PropfindJob::finishedWithError, this, &PropagateRemoteMkdir::propfindError);
        propfindJob->start();
        _job = propfindJob;
        return;
    }

    if (!_uploadEncryptedHelper && !_item->_isEncrypted) {
        success();
    } else {
        // We still need to mark that folder encrypted in case we were uploading it as encrypted one
        // Another scenario, is we are creating a new folder because of move operation on an encrypted folder that works via remove + re-upload
        propagator()->_activeJobList.append(this);

        // We're expecting directory path in /Foo/Bar convention...
        Q_ASSERT(jobPath.startsWith('/') && !jobPath.endsWith('/'));
        // But encryption job expect it in Foo/Bar/ convention
        auto job = new OCC::EncryptFolderJob(propagator()->account(), propagator()->_journal, jobPath.mid(1), _item->_fileId, this);
        connect(job, &OCC::EncryptFolderJob::finished, this, &PropagateRemoteMkdir::slotEncryptFolderFinished);
        job->start();
    }
}

void PropagateRemoteMkdir::slotMkdir()
{
    const auto path = _item->_file;
    const auto slashPosition = path.lastIndexOf('/');
    const auto parentPath = slashPosition >= 0 ? path.left(slashPosition) : QString();

    SyncJournalFileRecord parentRec;
    bool ok = propagator()->_journal->getFileRecord(parentPath, &parentRec);
    if (!ok) {
        done(SyncFileItem::NormalError);
        return;
    }

    if (!hasEncryptedAncestor()) {
        slotStartMkcolJob();
        return;
    }

    // We should be encrypted as well since our parent is
    const auto remoteParentPath = parentRec._e2eMangledName.isEmpty() ? parentPath : parentRec._e2eMangledName;
    _uploadEncryptedHelper = new PropagateUploadEncrypted(propagator(), remoteParentPath, _item, this);
    connect(_uploadEncryptedHelper, &PropagateUploadEncrypted::finalized,
      this, &PropagateRemoteMkdir::slotStartEncryptedMkcolJob);
    connect(_uploadEncryptedHelper, &PropagateUploadEncrypted::error,
      []{ qCDebug(lcPropagateRemoteMkdir) << "Error setting up encryption."; });
    _uploadEncryptedHelper->start();
}

void PropagateRemoteMkdir::slotMkcolJobFinished()
{
    propagator()->_activeJobList.removeOne(this);

    ASSERT(_job);

    QNetworkReply::NetworkError err = _job->reply()->error();
    _item->_httpErrorCode = _job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    _item->_responseTimeStamp = _job->responseTimestamp();
    _item->_requestId = _job->requestId();

    _item->_fileId = _job->reply()->rawHeader("OC-FileId");

    _item->_errorString = _job->errorString();

    const auto jobHttpReasonPhraseString = _job->reply()->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();

    const auto jobPath = _job->path();

    if (_uploadEncryptedHelper && _uploadEncryptedHelper->isFolderLocked() && !_uploadEncryptedHelper->isUnlockRunning()) {
        // since we are done, we need to unlock a folder in case it was locked
        connect(_uploadEncryptedHelper, &PropagateUploadEncrypted::folderUnlocked, this, [this, err, jobHttpReasonPhraseString, jobPath]() {
            finalizeMkColJob(err, jobHttpReasonPhraseString, jobPath);
        });
        _uploadEncryptedHelper->unlockFolder();
    } else {
        finalizeMkColJob(err, jobHttpReasonPhraseString, jobPath);
    }
}

void PropagateRemoteMkdir::slotEncryptFolderFinished()
{
    qCDebug(lcPropagateRemoteMkdir) << "Success making the new folder encrypted";
    propagator()->_activeJobList.removeOne(this);
    _item->_isEncrypted = true;
    success();
}

void PropagateRemoteMkdir::propfindResult(const QVariantMap &result)
{
    propagator()->_activeJobList.removeOne(this);
    if (result.contains("id")) {
        _item->_fileId = result["id"].toByteArray();
    }
    success();
}

void PropagateRemoteMkdir::propfindError()
{
    // ignore the PROPFIND error
    propagator()->_activeJobList.removeOne(this);
    done(SyncFileItem::Success);
}

void PropagateRemoteMkdir::success()
{
    // Never save the etag on first mkdir.
    // Only fully propagated directories should have the etag set.
    auto itemCopy = *_item;
    itemCopy._etag.clear();

    // save the file id already so we can detect rename or remove
    const auto result = propagator()->updateMetadata(itemCopy);
    if (!result) {
        done(SyncFileItem::FatalError, tr("Error writing metadata to the database: %1").arg(result.error()));
        return;
    } else if (*result == Vfs::ConvertToPlaceholderResult::Locked) {
        done(SyncFileItem::FatalError, tr("The file %1 is currently in use").arg(_item->_file));
        return;
    }

    done(SyncFileItem::Success);
}
}
