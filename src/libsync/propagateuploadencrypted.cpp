#include "propagateuploadencrypted.h"
#include "clientsideencryptionjobs.h"
#include "networkjobs.h"
#include "clientsideencryption.h"
#include "account.h"

#include <QFileInfo>
#include <QDir>
#include <QUrl>
#include <QFile>
#include <QTemporaryFile>
#include <QLoggingCategory>
#include <QMimeDatabase>

namespace OCC {

Q_LOGGING_CATEGORY(lcPropagateUploadEncrypted, "nextcloud.sync.propagator.upload.encrypted", QtInfoMsg)

PropagateUploadEncrypted::PropagateUploadEncrypted(OwncloudPropagator *propagator, SyncFileItemPtr item)
: _propagator(propagator),
 _item(item),
 _metadata(nullptr)
{
}

void PropagateUploadEncrypted::start()
{
  /* If the file is in a encrypted-enabled nextcloud instance, we need to
      * do the long road: Fetch the folder status of the encrypted bit,
      * if it's encrypted, find the ID of the folder.
      * lock the folder using it's id.
      * download the metadata
      * update the metadata
      * upload the file
      * upload the metadata
      * unlock the folder.
      *
      * If the folder is unencrypted we just follow the old way.
      */
      qCDebug(lcPropagateUploadEncrypted) << "Starting to send an encrypted file!";
      QFileInfo info(_item->_file);
      auto getEncryptedStatus = new GetFolderEncryptStatusJob(_propagator->account(),
                                                           info.path());

      connect(getEncryptedStatus, &GetFolderEncryptStatusJob::encryptStatusFolderReceived,
              this, &PropagateUploadEncrypted::slotFolderEncryptedStatusFetched);
      connect(getEncryptedStatus, &GetFolderEncryptStatusJob::encryptStatusError,
             this, &PropagateUploadEncrypted::slotFolderEncryptedStatusError);
      getEncryptedStatus->start();
}

void PropagateUploadEncrypted::slotFolderEncryptedStatusFetched(const QString &folder, bool isEncrypted)
{
  qCDebug(lcPropagateUploadEncrypted) << "Encrypted Status Fetched" << folder << isEncrypted;

  /* We are inside an encrypted folder, we need to find it's Id. */
  if (isEncrypted) {
      qCDebug(lcPropagateUploadEncrypted) << "Folder is encrypted, let's get the Id from it.";
      auto job = new LsColJob(_propagator->account(), folder, this);
      job->setProperties({"resourcetype", "http://owncloud.org/ns:fileid"});
      connect(job, &LsColJob::directoryListingSubfolders, this, &PropagateUploadEncrypted::slotFolderEncryptedIdReceived);
      connect(job, &LsColJob::finishedWithError, this, &PropagateUploadEncrypted::slotFolderEncryptedIdError);
      job->start();
  } else {
    qCDebug(lcPropagateUploadEncrypted) << "Folder is not encrypted, getting back to default.";
    emit folerNotEncrypted();
  }
}



/* We try to lock a folder, if it's locked we try again in one second.
 * if it's still locked we try again in one second. looping untill one minute.
 *                                                                      -> fail.
 * the 'loop':                                                         /
 *    slotFolderEncryptedIdReceived -> slotTryLock -> lockError -> stillTime? -> slotTryLock
 *                                        \
 *                                         -> success.
 */

void PropagateUploadEncrypted::slotFolderEncryptedIdReceived(const SyncObjectList &list)
{
  qCDebug(lcPropagateUploadEncrypted) << "Received id of folder, trying to lock it so we can prepare the metadata";
  auto job = qobject_cast<LsColJob *>(sender());
  const auto& folderInfo = job->_folderInfos.value(std::get<0>(list.at(0)));
  _folderLockFirstTry.start();
  slotTryLock(folderInfo.fileId);
}

void PropagateUploadEncrypted::slotTryLock(const QByteArray& fileId)
{
  auto *lockJob = new LockEncryptFolderApiJob(_propagator->account(), fileId, this);
  connect(lockJob, &LockEncryptFolderApiJob::success, this, &PropagateUploadEncrypted::slotFolderLockedSuccessfully);
  connect(lockJob, &LockEncryptFolderApiJob::error, this, &PropagateUploadEncrypted::slotFolderLockedError);
  lockJob->start();
}

void PropagateUploadEncrypted::slotFolderLockedSuccessfully(const QByteArray& fileId, const QByteArray& token)
{
  qCDebug(lcPropagateUploadEncrypted) << "Folder" << fileId << "Locked Successfully for Upload, Fetching Metadata";
  // Should I use a mutex here?
  _currentLockingInProgress = true;
  _folderToken = token;
  _folderId = fileId;

  auto job = new GetMetadataApiJob(_propagator->account(), _folderId);
  connect(job, &GetMetadataApiJob::jsonReceived,
          this, &PropagateUploadEncrypted::slotFolderEncryptedMetadataReceived);
  connect(job, &GetMetadataApiJob::error,
          this, &PropagateUploadEncrypted::slotFolderEncryptedMetadataError);

  job->start();
}

void PropagateUploadEncrypted::slotFolderEncryptedMetadataError(const QByteArray& fileId, int httpReturnCode)
{
    Q_UNUSED(fileId);
    Q_UNUSED(httpReturnCode);
    qCDebug(lcPropagateUploadEncrypted()) << "Error Getting the encrypted metadata. unlock the folder.";
    unlockFolder();
}

void PropagateUploadEncrypted::slotFolderEncryptedMetadataReceived(const QJsonDocument &json, int statusCode)
{
  qCDebug(lcPropagateUploadEncrypted) << "Metadata Received, Preparing it for the new file." << json.toVariant();

  // Encrypt File!
  _metadata = new FolderMetadata(_propagator->account(), json.toJson(QJsonDocument::Compact), statusCode);

  QFileInfo info(_propagator->_localDir + QDir::separator() + _item->_file);
  const QString fileName = info.fileName();

  // Find existing metadata for this file
  bool found = false;
  EncryptedFile encryptedFile;
  const QVector<EncryptedFile> files = _metadata->files();

  for(const EncryptedFile &file : files) {
    if (file.originalFilename == fileName) {
      encryptedFile = file;
      found = true;
    }
  }



  // New encrypted file so set it all up!
  if (!found) {
      encryptedFile.encryptionKey = EncryptionHelper::generateRandom(16);
      encryptedFile.encryptedFilename = EncryptionHelper::generateRandomFilename();
      encryptedFile.initializationVector = EncryptionHelper::generateRandom(16);
      encryptedFile.fileVersion = 1;
      encryptedFile.metadataKey = 1;
      encryptedFile.originalFilename = fileName;

      QMimeDatabase mdb;
      encryptedFile.mimetype = mdb.mimeTypeForFile(info).name().toLocal8Bit();
  }

  _item->_encryptedFileName = _item->_file.section(QLatin1Char('/'), 0, -2)
          + QLatin1Char('/') + encryptedFile.encryptedFilename;

  qCDebug(lcPropagateUploadEncrypted) << "Creating the encrypted file.";

  QFile input(info.absoluteFilePath());
  QFile output(QDir::tempPath() + QDir::separator() + encryptedFile.encryptedFilename);

  QByteArray tag;
  bool encryptionResult = EncryptionHelper::fileEncryption(
    encryptedFile.encryptionKey,
    encryptedFile.initializationVector,
    &input, &output, tag);

  if (!encryptionResult) {
    qCDebug(lcPropagateUploadEncrypted()) << "There was an error encrypting the file, aborting upload.";
    unlockFolder();
    return;
  }

  _completeFileName = output.fileName();

  qCDebug(lcPropagateUploadEncrypted) << "Creating the metadata for the encrypted file.";

  encryptedFile.authenticationTag = tag;

  _metadata->addEncryptedFile(encryptedFile);
  _encryptedFile = encryptedFile;

  qCDebug(lcPropagateUploadEncrypted) << "Metadata created, sending to the server.";

  if (statusCode == 404) {
    auto job = new StoreMetaDataApiJob(_propagator->account(),
                                       _folderId,
                                       _metadata->encryptedMetadata());
    connect(job, &StoreMetaDataApiJob::success, this, &PropagateUploadEncrypted::slotUpdateMetadataSuccess);
    connect(job, &StoreMetaDataApiJob::error, this, &PropagateUploadEncrypted::slotUpdateMetadataError);
    job->start();
  } else {
    auto job = new UpdateMetadataApiJob(_propagator->account(),
                                      _folderId,
                                      _metadata->encryptedMetadata(),
                                      _folderToken);

    connect(job, &UpdateMetadataApiJob::success, this, &PropagateUploadEncrypted::slotUpdateMetadataSuccess);
    connect(job, &UpdateMetadataApiJob::error, this, &PropagateUploadEncrypted::slotUpdateMetadataError);
    job->start();
  }
}

void PropagateUploadEncrypted::slotUpdateMetadataSuccess(const QByteArray& fileId)
{
    Q_UNUSED(fileId);
    qCDebug(lcPropagateUploadEncrypted) << "Uploading of the metadata success, Encrypting the file";
    QFileInfo outputInfo(_completeFileName);

    qCDebug(lcPropagateUploadEncrypted) << "Encrypted Info:" << outputInfo.path() << outputInfo.fileName() << outputInfo.size();
    qCDebug(lcPropagateUploadEncrypted) << "Finalizing the upload part, now the actuall uploader will take over";
    emit finalized(outputInfo.path() + QLatin1Char('/') + outputInfo.fileName(),
                   _item->_file.section(QLatin1Char('/'), 0, -2) + QLatin1Char('/') + outputInfo.fileName(),
                   outputInfo.size());
}

void PropagateUploadEncrypted::slotUpdateMetadataError(const QByteArray& fileId, int httpErrorResponse)
{
  qCDebug(lcPropagateUploadEncrypted) << "Update metadata error for folder" << fileId << "with error" << httpErrorResponse;
  qCDebug(lcPropagateUploadEncrypted()) << "Unlocking the folder.";
  unlockFolder();
}

void PropagateUploadEncrypted::slotFolderLockedError(const QByteArray& fileId, int httpErrorCode)
{
    Q_UNUSED(httpErrorCode);
    /* try to call the lock from 5 to 5 seconds
     * and fail if it's more than 5 minutes. */
    QTimer::singleShot(5000, this, [this, fileId]{
        if (!_currentLockingInProgress) {
            qCDebug(lcPropagateUploadEncrypted) << "Error locking the folder while no other update is locking it up.";
            qCDebug(lcPropagateUploadEncrypted) << "Perhaps another client locked it.";
            qCDebug(lcPropagateUploadEncrypted) << "Abort";
        return;
        }

        // Perhaps I should remove the elapsed timer if the lock is from this client?
        if (_folderLockFirstTry.elapsed() > /* five minutes */ 1000 * 60 * 5 ) {
            qCDebug(lcPropagateUploadEncrypted) << "One minute passed, ignoring more attemps to lock the folder.";
        return;
        }
        slotTryLock(fileId);
    });

    qCDebug(lcPropagateUploadEncrypted) << "Folder" << fileId << "Coundn't be locked.";
}

void PropagateUploadEncrypted::slotFolderEncryptedIdError(QNetworkReply *r)
{
    Q_UNUSED(r);
    qCDebug(lcPropagateUploadEncrypted) << "Error retrieving the Id of the encrypted folder.";
}

void PropagateUploadEncrypted::slotFolderEncryptedStatusError(int error)
{
    qCDebug(lcPropagateUploadEncrypted) << "Failed to retrieve the status of the folders." << error;
}

void PropagateUploadEncrypted::unlockFolder()
{
    qDebug() << "Calling Unlock";
    auto *unlockJob = new UnlockEncryptFolderApiJob(_propagator->account(),
        _folderId, _folderToken, this);

    connect(unlockJob, &UnlockEncryptFolderApiJob::success, []{ qDebug() << "Successfully Unlocked"; });
    connect(unlockJob, &UnlockEncryptFolderApiJob::error, []{ qDebug() << "Unlock Error"; });
    unlockJob->start();
}

} // namespace OCC
