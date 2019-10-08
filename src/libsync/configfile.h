/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#ifndef CONFIGFILE_H
#define CONFIGFILE_H

#include "owncloudlib.h"
#include <memory>
#include <QSharedPointer>
#include <QSettings>
#include <QString>
#include <QVariant>
#include <chrono>

class QWidget;
class QHeaderView;
class ExcludedFiles;

namespace OCC {

class AbstractCredentials;

/**
 * @brief The ConfigFile class
 * @ingroup libsync
 */
class OWNCLOUDSYNC_EXPORT ConfigFile
{
public:
    ConfigFile();

    enum Scope { UserScope,
        SystemScope };

    QString configPath() const;
    QString configFile() const;
    QString excludeFile(Scope scope) const;
    static QString excludeFileFromSystem(); // doesn't access config dir

    bool exists();

    QString defaultConnection() const;

    // the certs do not depend on a connection.
    QByteArray caCerts();
    void setCaCerts(const QByteArray &);

    bool passwordStorageAllowed(const QString &connection = QString());

    // max count of lines in the log window
    int maxLogLines() const;
    void setMaxLogLines(int);

    /* Server poll interval in milliseconds */
    std::chrono::milliseconds remotePollInterval(const QString &connection = QString()) const;
    /* Set poll interval. Value in milliseconds has to be larger than 5000 */
    void setRemotePollInterval(std::chrono::milliseconds interval, const QString &connection = QString());

    /* Interval to check for new notifications */
    std::chrono::milliseconds notificationRefreshInterval(const QString &connection = QString()) const;

    /* Force sync interval, in milliseconds */
    std::chrono::milliseconds forceSyncInterval(const QString &connection = QString()) const;

    /**
     * Interval in milliseconds within which full local discovery is required
     *
     * Use -1 to disable regular full local discoveries.
     */
    std::chrono::milliseconds fullLocalDiscoveryInterval() const;

    bool monoIcons() const;
    void setMonoIcons(bool);

    bool promptDeleteFiles() const;
    void setPromptDeleteFiles(bool promptDeleteFiles);

    bool crashReporter() const;
    void setCrashReporter(bool enabled);

    bool automaticLogDir() const;
    void setAutomaticLogDir(bool enabled);

    // proxy settings
    void setProxyType(int proxyType,
        const QString &host = QString(),
        int port = 0, bool needsAuth = false,
        const QString &user = QString(),
        const QString &pass = QString());

    int proxyType() const;
    QString proxyHostName() const;
    int proxyPort() const;
    bool proxyNeedsAuth() const;
    QString proxyUser() const;
    QString proxyPassword() const;

    /** 0: no limit, 1: manual, >0: automatic */
    int useUploadLimit() const;
    int useDownloadLimit() const;
    void setUseUploadLimit(int);
    void setUseDownloadLimit(int);
    /** in kbyte/s */
    int uploadLimit() const;
    int downloadLimit() const;
    void setUploadLimit(int kbytes);
    void setDownloadLimit(int kbytes);
    /** [checked, size in MB] **/
    QPair<bool, quint64> newBigFolderSizeLimit() const;
    void setNewBigFolderSizeLimit(bool isChecked, quint64 mbytes);
    bool confirmExternalStorage() const;
    void setConfirmExternalStorage(bool);

	/** Wi-Fi list manager configuration for black- or whitelisting**/
    void setWifiListActive(const bool &isActive);
    void setWifiListMode(const QString &wifiListMode);
    void setSsidList(const QStringList *wifiList);
    bool wifiListActive() const;
    QString wifiListMode() const;
    QStringList ssidList() const;

    /** If we should move the files deleted on the server in the trash  */
    bool moveToTrash() const;
    void setMoveToTrash(bool);

    static bool setConfDir(const QString &value);

    bool optionalServerNotifications() const;
    void setOptionalServerNotifications(bool show);

    bool showInExplorerNavigationPane() const;
    void setShowInExplorerNavigationPane(bool show);

    int timeout() const;
    quint64 chunkSize() const;
    quint64 maxChunkSize() const;
    quint64 minChunkSize() const;
    std::chrono::milliseconds targetChunkUploadDuration() const;

    void saveGeometry(QWidget *w);
    void restoreGeometry(QWidget *w);

    // how often the check about new versions runs
    std::chrono::milliseconds updateCheckInterval(const QString &connection = QString()) const;

    bool skipUpdateCheck(const QString &connection = QString()) const;
    void setSkipUpdateCheck(bool, const QString &);

    void saveGeometryHeader(QHeaderView *header);
    void restoreGeometryHeader(QHeaderView *header);

    QString certificatePath() const;
    void setCertificatePath(const QString &cPath);
    QString certificatePasswd() const;
    void setCertificatePasswd(const QString &cPasswd);

    /**  Returns a new settings pre-set in a specific group.  The Settings will be created
         with the given parent. If no parent is specified, the caller must destroy the settings */
    static std::unique_ptr<QSettings> settingsWithGroup(const QString &group, QObject *parent = nullptr);

    /// Add the system and user exclude file path to the ExcludedFiles instance.
    static void setupDefaultExcludeFilePaths(ExcludedFiles &excludedFiles);

protected:
    QVariant getPolicySetting(const QString &policy, const QVariant &defaultValue = QVariant()) const;
    void storeData(const QString &group, const QString &key, const QVariant &value);
    QVariant retrieveData(const QString &group, const QString &key) const;
    void removeData(const QString &group, const QString &key);
    bool dataExists(const QString &group, const QString &key) const;

private:
    QVariant getValue(const QString &param, const QString &group = QString(),
        const QVariant &defaultValue = QVariant()) const;
    void setValue(const QString &key, const QVariant &value);

private:
    typedef QSharedPointer<AbstractCredentials> SharedCreds;

    static bool _askedUser;
    static QString _oCVersion;
    static QString _confDir;
};
}
#endif // CONFIGFILE_H
