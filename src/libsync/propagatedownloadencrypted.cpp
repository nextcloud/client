#include "propagatedownloadencrypted.h"
#include "clientsideencryptionjobs.h"

Q_LOGGING_CATEGORY(lcPropagateDownloadEncrypted, "nextcloud.sync.propagator.download.encrypted", QtInfoMsg)


namespace OCC {

PropagateDownloadEncrypted::PropagateDownloadEncrypted(OwncloudPropagator *propagator, SyncFileItemPtr item) :
 _propagator(propagator), _item(item), _info(_item->_file)

{
}

void PropagateDownloadEncrypted::start() {
  checkFolderEncryptedStatus();
}

void PropagateDownloadEncrypted::checkFolderEncryptedStatus()
{
  auto getEncryptedStatus = new GetFolderEncryptStatusJob(_propagator->account(), _info.path());
  connect(getEncryptedStatus, &GetFolderEncryptStatusJob::encryptStatusFolderReceived,
          this, &PropagateDownloadEncrypted::folderStatusReceived);

  connect(getEncryptedStatus, &GetFolderEncryptStatusJob::encryptStatusError,
          this, &PropagateDownloadEncrypted::folderStatusError);

  getEncryptedStatus->start();
}

void PropagateDownloadEncrypted::folderStatusError(int statusCode)
{
  qCDebug(lcPropagateDownloadEncrypted) << "Failed to get encrypted status of folder" << statusCode;
}

void PropagateDownloadEncrypted::folderStatusReceived(const QString &folder, bool isEncrypted)
{
  qCDebug(lcPropagateDownloadEncrypted) << "Get Folder is Encrypted Received" << folder << isEncrypted;
  if (!isEncrypted) {
      emit folderStatusNotEncrypted();
      return;
  }

  // Is encrypted Now we need the folder-id
  auto job = new LsColJob(_propagator->account(), folder, this);
  job->setProperties({"resourcetype", "http://owncloud.org/ns:fileid"});
  connect(job, &LsColJob::directoryListingSubfolders,
          this, &PropagateDownloadEncrypted::checkFolderId);
  connect(job, &LsColJob::finishedWithError,
          this, &PropagateDownloadEncrypted::folderIdError);
  job->start();
}

void PropagateDownloadEncrypted::folderIdError()
{
  qCDebug(lcPropagateDownloadEncrypted) << "Failed to get encrypted metadata of folder";
}

void PropagateDownloadEncrypted::checkFolderId(const QStringList &list)
{
  auto job = qobject_cast<LsColJob*>(sender());
  const QString folderId = list.first();
  qCDebug(lcPropagateDownloadEncrypted) << "Received id of folder" << folderId;

  const ExtraFolderInfo &folderInfo = job->_folderInfos.value(folderId);

  // Now that we have the folder-id we need it's JSON metadata
  auto metadataJob = new GetMetadataApiJob(_propagator->account(), folderInfo.fileId);
  connect(metadataJob, &GetMetadataApiJob::jsonReceived,
          this, &PropagateDownloadEncrypted::checkFolderEncryptedMetadata);

  metadataJob->start();
}

void PropagateDownloadEncrypted::checkFolderEncryptedMetadata(const QJsonDocument &json)
{
  qCDebug(lcPropagateDownloadEncrypted) << "Metadata Received reading" << json.toJson() << _item->_file;
  const QString filename = _info.fileName();
  auto meta = new FolderMetadata(_propagator->account(), json.toJson(QJsonDocument::Compact));
  const QVector<EncryptedFile> files = meta->files();
  for (const EncryptedFile &file : files) {
    qCDebug(lcPropagateDownloadEncrypted) << "file" << filename << file.encryptedFilename << file.originalFilename << file.encryptionKey;
    if (filename == file.encryptedFilename) {
      _encryptedInfo = file;
      qCDebug(lcPropagateDownloadEncrypted) << "Found matching encrypted metadata for file, starting download";
      emit folderStatusEncrypted();
      return;
    }
  }

  qCDebug(lcPropagateDownloadEncrypted) << "Failed to find encrypted metadata information of remote file" << filename;
}

// TODO: Fix this. Exported in the wrong place.
QString createDownloadTmpFileName(const QString &previous);

bool PropagateDownloadEncrypted::decryptFile(QFile& tmpFile)
{
    const QString tmpFileName = createDownloadTmpFileName(_item->_file + QLatin1String("_dec"));
    qCDebug(lcPropagateDownloadEncrypted) << "Content Checksum Computed starting decryption" << tmpFileName;

    tmpFile.close();
    auto _tmpOutput = new QFile(_propagator->getFilePath(tmpFileName), this);
    EncryptionHelper::fileDecryption(_encryptedInfo.encryptionKey,
                                      _encryptedInfo.initializationVector,
                                      &tmpFile,
                                      _tmpOutput);

    qCDebug(lcPropagateDownloadEncrypted) << "Decryption finished" << tmpFile.fileName() << _tmpOutput->fileName();

    tmpFile.close();
    _tmpOutput->close();

    // we decripted the temporary into another temporary, so good bye old one
    if (!tmpFile.remove()) {
        qCDebug(lcPropagateDownloadEncrypted) << "Failed to remove temporary file" << tmpFile.errorString();
        _errorString = tmpFile.errorString();
        return false;
    }

    // Let's fool the rest of the logic into thinking this was the actual download
    tmpFile.setFileName(_tmpOutput->fileName());

    // all encrypted files need to be renamed after
    _item->_renameTarget = _item->_file.section(QLatin1Char('/'), 0, -2)
      + QLatin1Char('/') + _encryptedInfo.originalFilename;

    _item->_isEndToEndEncrypted = true;
    _item->_encryptedFileName = _encryptedInfo.encryptedFilename;

    return true;
}

QString PropagateDownloadEncrypted::errorString() const
{
  return _errorString;
}

}
