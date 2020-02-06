#ifndef PROPAGATEDOWNLOADENCRYPTED_H
#define PROPAGATEDOWNLOADENCRYPTED_H

#include <QObject>
#include <QFileInfo>

#include "syncfileitem.h"
#include "owncloudpropagator.h"
#include "clientsideencryption.h"

class QJsonDocument;

namespace OCC {

class PropagateDownloadEncrypted : public QObject {
  Q_OBJECT
public:
  PropagateDownloadEncrypted(OwncloudPropagator *propagator, SyncFileItemPtr item);
  void start();
  void checkFolderId(const SyncObjectList &list);
  bool decryptFile(QFile& tmpFile);
  QString errorString() const;

public slots:
  void checkFolderEncryptedStatus();

  void checkFolderEncryptedMetadata(const QJsonDocument &json);
  void folderStatusReceived(const QString &folder, bool isEncrypted);
  void folderStatusError(int httpErrorCode);
  void folderIdError();
signals:
  void folderStatusEncrypted();
  void folderStatusNotEncrypted();
  void failed();

  void decryptionFinished();

private:
  OwncloudPropagator *_propagator;
  SyncFileItemPtr _item;
  QFileInfo _info;
  EncryptedFile _encryptedInfo;
  QString _errorString;
};

}
#endif
