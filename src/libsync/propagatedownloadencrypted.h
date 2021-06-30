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
  PropagateDownloadEncrypted(OwncloudPropagator *propagator, const QString &localParentPath, SyncFileItemPtr item, QObject *parent = nullptr);
  void start();
  bool decryptFile(QFile& tmpFile);
  QString errorString() const;

public slots:
  void checkFolderId(const QStringList &list);
  void checkFolderEncryptedMetadata(const QJsonDocument &json);
  void folderIdError();
  void folderEncryptedMetadataError(const QByteArray &fileId, int httpReturnCode);

signals:
  void fileMetadataFound();
  void failed();

  void decryptionFinished();

private:
  OwncloudPropagator *_propagator;
  QString _localParentPath;
  SyncFileItemPtr _item;
  QFileInfo _info;
  EncryptedFile _encryptedInfo;
  QString _errorString;
};

}
#endif
