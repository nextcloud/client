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

#include "propagateremotemove.h"
#include "propagatorjobs.h"
#include "owncloudpropagator_p.h"
#include "account.h"
#include "common/syncjournalfilerecord.h"
#include "filesystem.h"
#include "common/asserts.h"
#include <QFile>
#include <QStringList>
#include <QDir>

namespace OCC {

Q_LOGGING_CATEGORY(lcMoveJob, "nextcloud.sync.networkjob.move", QtInfoMsg)
Q_LOGGING_CATEGORY(lcPropagateRemoteMove, "nextcloud.sync.propagator.remotemove", QtInfoMsg)

MoveJob::MoveJob(AccountPtr account, const QString &path,
    const QString &destination, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
    , _destination(destination)
{
}

MoveJob::MoveJob(AccountPtr account, const QUrl &url, const QString &destination,
    QMap<QByteArray, QByteArray> extraHeaders, QObject *parent)
    : AbstractNetworkJob(account, QString(), parent)
    , _destination(destination)
    , _url(url)
    , _extraHeaders(extraHeaders)
{
}

void MoveJob::start()
{
    QNetworkRequest req;
    req.setRawHeader("Destination", QUrl::toPercentEncoding(_destination, "/"));
    for (auto it = _extraHeaders.constBegin(); it != _extraHeaders.constEnd(); ++it) {
        req.setRawHeader(it.key(), it.value());
    }
    if (_url.isValid()) {
        sendRequest("MOVE", _url, req);
    } else {
        sendRequest("MOVE", makeDavUrl(path()), req);
    }

    if (reply()->error() != QNetworkReply::NoError) {
        qCWarning(lcPropagateRemoteMove) << " Network error: " << reply()->errorString();
    }
    AbstractNetworkJob::start();
}


bool MoveJob::finished()
{
    qCInfo(lcMoveJob) << "MOVE of" << reply()->request().url() << "FINISHED WITH STATUS"
                      << replyStatusString();

    emit finishedSignal();
    return true;
}

void PropagateRemoteMove::start()
{
    if (propagator()->_abortRequested)
        return;

    QString origin = propagator()->adjustRenamedPath(_item->_file);
    qCDebug(lcPropagateRemoteMove) << origin << _item->_renameTarget;

    QString targetFile(propagator()->fullLocalPath(_item->_renameTarget));

    if (origin == _item->_renameTarget) {
        // The parent has been renamed already so there is nothing more to do.

        if (!_item->_encryptedFileName.isEmpty()) {
            // when renaming non-encrypted folder that contains encrypted folder, nested files of its encrypted folder are incorrectly displayed in the Settings dialog
            // encrypted name is displayed instead of a local folder name, unless the sync folder is removed, then added again and re-synced
            // we are fixing it by modifying the "_encryptedFileName" in such a way so it will have a renamed root path at the beginning of it as expected
            // corrected "_encryptedFileName" is later used in propagator()->updateMetadata() call that will update the record in the Sync journal DB

            const auto path = _item->_file;
            const auto slashPosition = path.lastIndexOf('/');
            const auto parentPath = slashPosition >= 0 ? path.left(slashPosition) : QString();

            SyncJournalFileRecord parentRec;
            bool ok = propagator()->_journal->getFileRecord(parentPath, &parentRec);
            if (!ok) {
                done(SyncFileItem::NormalError);
                return;
            }

            const auto remoteParentPath = parentRec._e2eMangledName.isEmpty() ? parentPath : parentRec._e2eMangledName;

            const auto lastSlashPosition = _item->_encryptedFileName.lastIndexOf('/');
            const auto encryptedName = lastSlashPosition >= 0 ? _item->_encryptedFileName.mid(lastSlashPosition + 1) : QString();

            if (!encryptedName.isEmpty()) {
                _item->_encryptedFileName = remoteParentPath + "/" + encryptedName;
            }
        }

        finalize();
        return;
    }

    QString remoteSource = propagator()->fullRemotePath(origin);
    QString remoteDestination = QDir::cleanPath(propagator()->account()->davUrl().path() + propagator()->fullRemotePath(_item->_renameTarget));

    auto &vfs = propagator()->syncOptions()._vfs;
    auto itype = _item->_type;
    ASSERT(itype != ItemTypeVirtualFileDownload && itype != ItemTypeVirtualFileDehydration);
    if (vfs->mode() == Vfs::WithSuffix && itype != ItemTypeDirectory) {
        const auto suffix = vfs->fileSuffix();
        bool sourceHadSuffix = remoteSource.endsWith(suffix);
        bool destinationHadSuffix = remoteDestination.endsWith(suffix);

        // Remote source and destination definitely shouldn't have the suffix
        if (sourceHadSuffix)
            remoteSource.chop(suffix.size());
        if (destinationHadSuffix)
            remoteDestination.chop(suffix.size());

        QString folderTarget = _item->_renameTarget;

        // Users can rename the file *and at the same time* add or remove the vfs
        // suffix. That's a complicated case where a remote rename plus a local hydration
        // change is requested. We don't currently deal with that. Instead, the rename
        // is propagated and the local vfs suffix change is reverted.
        // The discovery would still set up _renameTarget without the changed
        // suffix, since that's what must be propagated to the remote but the local
        // file may have a different name. folderTargetAlt will contain this potential
        // name.
        QString folderTargetAlt = folderTarget;
        if (itype == ItemTypeFile) {
            ASSERT(!sourceHadSuffix && !destinationHadSuffix);

            // If foo -> bar.owncloud, the rename target will be "bar"
            folderTargetAlt = folderTarget + suffix;

        } else if (itype == ItemTypeVirtualFile) {
            ASSERT(sourceHadSuffix && destinationHadSuffix);

            // If foo.owncloud -> bar, the rename target will be "bar.owncloud"
            folderTargetAlt.chop(suffix.size());
        }

        QString localTarget = propagator()->fullLocalPath(folderTarget);
        QString localTargetAlt = propagator()->fullLocalPath(folderTargetAlt);

        // If the expected target doesn't exist but a file with different hydration
        // state does, rename the local file to bring it in line with what the discovery
        // has set up.
        if (!FileSystem::fileExists(localTarget) && FileSystem::fileExists(localTargetAlt)) {
            QString error;
            if (!FileSystem::uncheckedRenameReplace(localTargetAlt, localTarget, &error)) {
                done(SyncFileItem::NormalError, tr("Could not rename %1 to %2, error: %3")
                     .arg(folderTargetAlt, folderTarget, error));
                return;
            }
            qCInfo(lcPropagateRemoteMove) << "Suffix vfs required local rename of"
                                          << folderTargetAlt << "to" << folderTarget;
        }
    }
    qCDebug(lcPropagateRemoteMove) << remoteSource << remoteDestination;

    _job = new MoveJob(propagator()->account(), remoteSource, remoteDestination, this);
    connect(_job.data(), &MoveJob::finishedSignal, this, &PropagateRemoteMove::slotMoveJobFinished);
    propagator()->_activeJobList.append(this);
    _job->start();
}

void PropagateRemoteMove::abort(PropagatorJob::AbortType abortType)
{
    if (_job && _job->reply())
        _job->reply()->abort();

    if (abortType == AbortType::Asynchronous) {
        emit abortFinished();
    }
}

void PropagateRemoteMove::slotMoveJobFinished()
{
    propagator()->_activeJobList.removeOne(this);

    ASSERT(_job);

    QNetworkReply::NetworkError err = _job->reply()->error();
    _item->_httpErrorCode = _job->reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    _item->_responseTimeStamp = _job->responseTimestamp();
    _item->_requestId = _job->requestId();

    if (err != QNetworkReply::NoError) {
        SyncFileItem::Status status = classifyError(err, _item->_httpErrorCode,
            &propagator()->_anotherSyncNeeded);
        done(status, _job->errorString());
        return;
    }

    if (_item->_httpErrorCode != 201) {
        // Normally we expect "201 Created"
        // If it is not the case, it might be because of a proxy or gateway intercepting the request, so we must
        // throw an error.
        done(SyncFileItem::NormalError,
            tr("Wrong HTTP code returned by server. Expected 201, but received \"%1 %2\".")
                .arg(_item->_httpErrorCode)
                .arg(_job->reply()->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString()));
        return;
    }

    finalize();
}

void PropagateRemoteMove::finalize()
{
    // Retrieve old db data.
    // if reading from db failed still continue hoping that deleteFileRecord
    // reopens the db successfully.
    // The db is only queried to transfer the content checksum from the old
    // to the new record. It is not a problem to skip it here.
    SyncJournalFileRecord oldRecord;
    propagator()->_journal->getFileRecord(_item->_originalFile, &oldRecord);
    auto &vfs = propagator()->syncOptions()._vfs;
    auto pinState = vfs->pinState(_item->_originalFile);

    // Delete old db data.
    propagator()->_journal->deleteFileRecord(_item->_originalFile);
    vfs->setPinState(_item->_originalFile, PinState::Inherited);

    SyncFileItem newItem(*_item);
    newItem._type = _item->_type;
    if (oldRecord.isValid()) {
        newItem._checksumHeader = oldRecord._checksumHeader;
        if (newItem._size != oldRecord._fileSize) {
            qCWarning(lcPropagateRemoteMove) << "File sizes differ on server vs sync journal: " << newItem._size << oldRecord._fileSize;

            // the server might have claimed a different size, we take the old one from the DB
            newItem._size = oldRecord._fileSize;
        }
    }
    const auto result = propagator()->updateMetadata(newItem);
    if (!result) {
        done(SyncFileItem::FatalError, tr("Error updating metadata: %1").arg(result.error()));
        return;
    } else if (*result == Vfs::ConvertToPlaceholderResult::Locked) {
        done(SyncFileItem::SoftError, tr("The file %1 is currently in use").arg(newItem._file));
        return;
    }
    if (pinState && *pinState != PinState::Inherited
        && !vfs->setPinState(newItem._renameTarget, *pinState)) {
        done(SyncFileItem::NormalError, tr("Error setting pin state"));
        return;
    }

    if (_item->isDirectory()) {
        propagator()->_renamedDirectories.insert(_item->_file, _item->_renameTarget);
        if (!adjustSelectiveSync(propagator()->_journal, _item->_file, _item->_renameTarget)) {
            done(SyncFileItem::FatalError, tr("Error writing metadata to the database"));
            return;
        }
    }

    propagator()->_journal->commit("Remote Rename");
    done(SyncFileItem::Success);
}

bool PropagateRemoteMove::adjustSelectiveSync(SyncJournalDb *journal, const QString &from_, const QString &to_)
{
    bool ok = false;
    // We only care about preserving the blacklist.   The white list should anyway be empty.
    // And the undecided list will be repopulated on the next sync, if there is anything too big.
    QStringList list = journal->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, &ok);
    if (!ok)
        return false;

    bool changed = false;
    ASSERT(!from_.endsWith(QLatin1String("/")));
    ASSERT(!to_.endsWith(QLatin1String("/")));
    QString from = from_ + QLatin1String("/");
    QString to = to_ + QLatin1String("/");

    for (auto &s : list) {
        if (s.startsWith(from)) {
            s = s.replace(0, from.size(), to);
            changed = true;
        }
    }

    if (changed) {
        journal->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, list);
    }
    return true;
}
}
