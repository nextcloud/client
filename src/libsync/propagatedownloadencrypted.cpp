#include "propagatedownloadencrypted.h"
#include "clientsideencryptionjobs.h"

Q_LOGGING_CATEGORY(lcPropagateDownloadEncrypted, "nextcloud.sync.propagator.download.encrypted", QtInfoMsg)


namespace OCC {

PropagateDownloadEncrypted::PropagateDownloadEncrypted(OwncloudPropagator *propagator, const QString &localParentPath, SyncFileItemPtr item, QObject *parent)
    : QObject(parent)
    , _propagator(propagator)
    , _localParentPath(localParentPath)
    , _item(item)
    , _info(_item->_file)
{
}

void PropagateDownloadEncrypted::start()
{
    const auto rootPath = [=]() {
        const auto result = _propagator->remotePath();
        if (result.startsWith('/')) {
            return result.mid(1);
        } else {
            return result;
        }
    }();
    const auto remoteFilename = _item->_encryptedFileName.isEmpty() ? _item->_file : _item->_encryptedFileName;
    const auto remotePath = QString(rootPath + remoteFilename);
    const auto remoteParentPath = remotePath.left(remotePath.lastIndexOf('/'));

    // Is encrypted Now we need the folder-id
    auto job = new LsColJob(_propagator->account(), remoteParentPath, this);
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
  connect(metadataJob, &GetMetadataApiJob::error,
          this, &PropagateDownloadEncrypted::folderEncryptedMetadataError);

  metadataJob->start();
}

void PropagateDownloadEncrypted::folderEncryptedMetadataError(const QByteArray & /*fileId*/, int /*httpReturnCode*/)
{
    qCCritical(lcPropagateDownloadEncrypted) << "Failed to find encrypted metadata information of remote file" << _info.fileName();
    emit failed();
}

void PropagateDownloadEncrypted::checkFolderEncryptedMetadata(const QJsonDocument &json)
{
  qCDebug(lcPropagateDownloadEncrypted) << "Metadata Received reading"
                                        << _item->_instruction << _item->_file << _item->_encryptedFileName;
  const QString filename = _info.fileName();
  auto meta = new FolderMetadata(_propagator->account(), json.toJson(QJsonDocument::Compact));
  const QVector<EncryptedFile> files = meta->files();

  const QString encryptedFilename = _item->_encryptedFileName.section(QLatin1Char('/'), -1);
  for (const EncryptedFile &file : files) {
    if (encryptedFilename == file.encryptedFilename) {
      _encryptedInfo = file;

      qCDebug(lcPropagateDownloadEncrypted) << "Found matching encrypted metadata for file, starting download";
      emit fileMetadataFound();
      return;
    }
  }

  emit failed();
  qCCritical(lcPropagateDownloadEncrypted) << "Failed to find encrypted metadata information of remote file" << filename;
}

// TODO: Fix this. Exported in the wrong place.
QString createDownloadTmpFileName(const QString &previous);

bool PropagateDownloadEncrypted::decryptFile(QFile& tmpFile)
{
    const QString tmpFileName = createDownloadTmpFileName(_item->_file + QLatin1String("_dec"));
    qCDebug(lcPropagateDownloadEncrypted) << "Content Checksum Computed starting decryption" << tmpFileName;

    tmpFile.close();
    QFile _tmpOutput(_propagator->fullLocalPath(tmpFileName), this);
    EncryptionHelper::fileDecryption(_encryptedInfo.encryptionKey,
                                     _encryptedInfo.initializationVector,
                                     &tmpFile,
                                     &_tmpOutput);

    qCDebug(lcPropagateDownloadEncrypted) << "Decryption finished" << tmpFile.fileName() << _tmpOutput.fileName();

    tmpFile.close();
    _tmpOutput.close();

    // we decripted the temporary into another temporary, so good bye old one
    if (!tmpFile.remove()) {
        qCDebug(lcPropagateDownloadEncrypted) << "Failed to remove temporary file" << tmpFile.errorString();
        _errorString = tmpFile.errorString();
        return false;
    }

    // Let's fool the rest of the logic into thinking this was the actual download
    tmpFile.setFileName(_tmpOutput.fileName());

    return true;
}

QString PropagateDownloadEncrypted::errorString() const
{
  return _errorString;
}

int PropagateDownloadEncrypted::authenticationTagSize() const
{
    return _encryptedInfo.authenticationTag.size();
}

}
