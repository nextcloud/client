#include "propagateremotedeleteencrypted.h"
#include "clientsideencryptionjobs.h"
#include "clientsideencryption.h"
#include "owncloudpropagator.h"

#include <QLoggingCategory>
#include <QMimeDatabase>
#include <QFileInfo>
#include <QDir>

using namespace OCC;

Q_LOGGING_CATEGORY(PROPAGATE_REMOVE_ENCRYPTED, "nextcloud.sync.propagator.remove.encrypted")

PropagateRemoteDeleteEncrypted::PropagateRemoteDeleteEncrypted(OwncloudPropagator *propagator, SyncFileItemPtr item, QObject *parent)
    : QObject(parent)
    , _propagator(propagator)
    , _item(item)
{

}

void PropagateRemoteDeleteEncrypted::start()
{
    QFileInfo info(_item->_file);
    qCDebug(PROPAGATE_REMOVE_ENCRYPTED) << "Folder is encrypted, let's get the Id from it.";
    auto job = new LsColJob(_propagator->account(), info.path(), this);
    job->setProperties({"resourcetype", "http://owncloud.org/ns:fileid"});
    connect(job, &LsColJob::directoryListingSubfolders, this, &PropagateRemoteDeleteEncrypted::slotFolderEncryptedIdReceived);
    connect(job, &LsColJob::finishedWithError, this, &PropagateRemoteDeleteEncrypted::taskFailed);
    job->start();
}

void PropagateRemoteDeleteEncrypted::slotFolderEncryptedIdReceived(const SyncObjectList &list)
{
    qCDebug(PROPAGATE_REMOVE_ENCRYPTED) << "Received id of folder, trying to lock it so we can prepare the metadata";
    auto job = qobject_cast<LsColJob *>(sender());
    const ExtraFolderInfo folderInfo = job->_folderInfos.value(std::get<0>(list.at(0)));
    slotTryLock(folderInfo.fileId);
}

void PropagateRemoteDeleteEncrypted::slotTryLock(const QByteArray &folderId)
{
    auto lockJob = new LockEncryptFolderApiJob(_propagator->account(), folderId, this);
    connect(lockJob, &LockEncryptFolderApiJob::success, this, &PropagateRemoteDeleteEncrypted::slotFolderLockedSuccessfully);
    connect(lockJob, &LockEncryptFolderApiJob::error, this, &PropagateRemoteDeleteEncrypted::taskFailed);
    lockJob->start();
}

void PropagateRemoteDeleteEncrypted::slotFolderLockedSuccessfully(const QByteArray &folderId, const QByteArray &token)
{
    qCDebug(PROPAGATE_REMOVE_ENCRYPTED) << "Folder id" << folderId << "Locked Successfully for Upload, Fetching Metadata";
    _folderLocked = true;
    _folderToken = token;
    _folderId = folderId;

    auto job = new GetMetadataApiJob(_propagator->account(), _folderId);
    connect(job, &GetMetadataApiJob::jsonReceived, this, &PropagateRemoteDeleteEncrypted::slotFolderEncryptedMetadataReceived);
    connect(job, &GetMetadataApiJob::error, this, &PropagateRemoteDeleteEncrypted::taskFailed);
    job->start();
}

void PropagateRemoteDeleteEncrypted::slotFolderEncryptedMetadataReceived(const QJsonDocument &json, int statusCode)
{
    if (statusCode == 404) {
        qCDebug(PROPAGATE_REMOVE_ENCRYPTED) << "Metadata not found, ignoring.";
        unlockFolder();
        return;
    }

    qCDebug(PROPAGATE_REMOVE_ENCRYPTED) << "Metadata Received, Preparing it for the new file.";

    // Encrypt File!
    FolderMetadata metadata(_propagator->account(), json.toJson(QJsonDocument::Compact), statusCode);

    QFileInfo info(_propagator->_localDir + QDir::separator() + _item->_file);
    const QString fileName = info.fileName();

    // Find existing metadata for this file
    bool found = false;
    const QVector<EncryptedFile> files = metadata.files();
    for (const EncryptedFile &file : files) {
        if (file.encryptedFilename == fileName) {
            metadata.removeEncryptedFile(file);
            found = true;
            break;
        }
    }

    if (!found) {
        // The removed file was not in the JSON so nothing else to do
        unlockFolder();
    }

    qCDebug(PROPAGATE_REMOVE_ENCRYPTED) << "Metadata updated, sending to the server.";

    auto job = new UpdateMetadataApiJob(_propagator->account(),
                                        _folderId,
                                        metadata.encryptedMetadata(),
                                        _folderToken);

    connect(job, &UpdateMetadataApiJob::success, this, &PropagateRemoteDeleteEncrypted::unlockFolder);
    connect(job, &UpdateMetadataApiJob::error, this, &PropagateRemoteDeleteEncrypted::taskFailed);
    job->start();
}

void PropagateRemoteDeleteEncrypted::unlockFolder()
{
    qCDebug(PROPAGATE_REMOVE_ENCRYPTED) << "Unlocking folder" << _folderId;
    auto unlockJob = new UnlockEncryptFolderApiJob(_propagator->account(),
                                                   _folderId, _folderToken, this);

    connect(unlockJob, &UnlockEncryptFolderApiJob::success, [this] {
        qCDebug(PROPAGATE_REMOVE_ENCRYPTED) << "Folder successfully unlocked" << _folderId;
        _folderLocked = false;
        emit finished(true);
    });
    connect(unlockJob, &UnlockEncryptFolderApiJob::error, this, &PropagateRemoteDeleteEncrypted::taskFailed);
    unlockJob->start();
}

void PropagateRemoteDeleteEncrypted::taskFailed()
{
    qCDebug(PROPAGATE_REMOVE_ENCRYPTED) << "Task failed of job" << sender();
    if (_folderLocked) {
        unlockFolder();
    } else {
        emit finished(false);
    }
}
