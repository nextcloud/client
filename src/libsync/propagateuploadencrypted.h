#ifndef PROPAGATEUPLOADENCRYPTED_H
#define PROPAGATEUPLOADENCRYPTED_H

#include <QObject>
#include <QString>
#include <QMap>
#include <QByteArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QFile>
#include <QTemporaryFile>

#include "owncloudpropagator.h"
#include "clientsideencryption.h"

namespace OCC {
class FolderMetadata;

  /* This class is used if the server supports end to end encryption.
 * It will fire for *any* folder, encrypted or not, because when the
 * client starts the upload request we don't know if the folder is
 * encrypted on the server.
 *
 * emits:
 * finalized() if the encrypted file is ready to be uploaded
 * error() if there was an error with the encryption
 * folerNotEncrypted() if the file is within a folder that's not encrypted.
 *
 */

class PropagateUploadEncrypted : public QObject
{
  Q_OBJECT
public:
    PropagateUploadEncrypted(OwncloudPropagator *propagator, SyncFileItemPtr item);
    void start();

    /* unlocks the current folder that holds this file */
    void unlockFolder();
  // Used by propagateupload
  QByteArray _folderToken;
  QByteArray _folderId;

private slots:
    void slotFolderEncryptedStatusFetched(const QString &folder, bool isEncrypted);
    void slotFolderEncryptedStatusError(int error);
    void slotFolderEncryptedIdReceived(const SyncObjectList &list);
    void slotFolderEncryptedIdError(QNetworkReply *r);
    void slotFolderLockedSuccessfully(const QByteArray& fileId, const QByteArray& token);
    void slotFolderLockedError(const QByteArray& fileId, int httpErrorCode);
    void slotTryLock(const QByteArray& fileId);
    void slotFolderEncryptedMetadataReceived(const QJsonDocument &json, int statusCode);
    void slotFolderEncryptedMetadataError(const QByteArray& fileId, int httpReturnCode);
    void slotUpdateMetadataSuccess(const QByteArray& fileId);
    void slotUpdateMetadataError(const QByteArray& fileId, int httpReturnCode);

signals:
    // Emmited after the file is encrypted and everythign is setup.
    void finalized(const QString& path, const QString& filename, quint64 size);
    void error();

    // Emited if the file is not in a encrypted folder.
    void folerNotEncrypted();

private:
  OwncloudPropagator *_propagator;
  SyncFileItemPtr _item;

  QElapsedTimer _folderLockFirstTry;
  bool _currentLockingInProgress = false;

  QByteArray _generatedKey;
  QByteArray _generatedIv;
  FolderMetadata *_metadata;
  EncryptedFile _encryptedFile;
  QString _completeFileName;
};


}
#endif
