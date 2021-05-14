/*
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
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


#ifndef SERVERCONNECTION_H
#define SERVERCONNECTION_H

#include <QByteArray>
#include <QUrl>
#include <QNetworkCookie>
#include <QNetworkRequest>
#include <QSslSocket>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslCipher>
#include <QSslError>
#include <QSharedPointer>

#ifndef TOKEN_AUTH_ONLY
#include <QPixmap>
#endif

#include "common/utility.h"
#include <memory>
#include "capabilities.h"
#include "clientsideencryption.h"
#include "jobqueue.h"

class QSettings;
class QNetworkReply;
class QUrl;
class QNetworkAccessManager;

namespace QKeychain {
class Job;
class WritePasswordJob;
class ReadPasswordJob;
}

namespace OCC {

class AbstractCredentials;
class Account;
using AccountPtr = QSharedPointer<Account>;
class AccessManager;
class SimpleNetworkJob;
class PushNotifications;

/**
 * @brief Reimplement this to handle SSL errors from libsync
 * @ingroup libsync
 */
class AbstractSslErrorHandler
{
public:
    virtual ~AbstractSslErrorHandler() = default;
    virtual bool handleErrors(QList<QSslError>, const QSslConfiguration &conf, QList<QSslCertificate> *, AccountPtr) = 0;
};

/**
 * @brief The Account class represents an account on an ownCloud Server
 * @ingroup libsync
 *
 * The Account has a name and url. It also has information about credentials,
 * SSL errors and certificates.
 */
class OWNCLOUDSYNC_EXPORT Account : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString id MEMBER _id)
    Q_PROPERTY(QString davUser MEMBER _davUser)
    Q_PROPERTY(QString displayName MEMBER _displayName)
    Q_PROPERTY(QUrl url MEMBER _url)

public:
    static AccountPtr create();
    ~Account();

    AccountPtr sharedFromThis();

    /**
     * The user that can be used in dav url.
     *
     * This can very well be different frome the login user that's
     * stored in credentials()->user().
     */
    QString davUser() const;
    void setDavUser(const QString &newDavUser);

    QString davDisplayName() const;
    void setDavDisplayName(const QString &newDisplayName);

#ifndef TOKEN_AUTH_ONLY
    QImage avatar() const;
    void setAvatar(const QImage &img);
#endif

    /// The name of the account as shown in the toolbar
    QString displayName() const;

    /// The internal id of the account.
    QString id() const;

    /** Server url of the account */
    void setUrl(const QUrl &url);
    QUrl url() const { return _url; }

    /// Adjusts _userVisibleUrl once the host to use is discovered.
    void setUserVisibleHost(const QString &host);

    /**
     * @brief The possibly themed dav path for the account. It has
     *        a trailing slash.
     * @returns the (themeable) dav path for the account.
     */
    QString davPath() const;
    void setDavPath(const QString &s) { _davPath = s; }
    void setNonShib(bool nonShib);

    /** Returns webdav entry URL, based on url() */
    QUrl davUrl() const;

    /** Returns the legacy permalink url for a file.
     *
     * This uses the old way of manually building the url. New code should
     * use the "privatelink" property accessible via PROPFIND.
     */
    QUrl deprecatedPrivateLinkUrl(const QByteArray &numericFileId) const;

    /** Holds the accounts credentials */
    AbstractCredentials *credentials() const;
    void setCredentials(AbstractCredentials *cred);

    /** Create a network request on the account's QNAM.
     *
     * Network requests in AbstractNetworkJobs are created through
     * this function. Other places should prefer to use jobs or
     * sendRequest().
     */
    QNetworkReply *sendRawRequest(const QByteArray &verb,
        const QUrl &url,
        QNetworkRequest req = QNetworkRequest(),
        QIODevice *data = nullptr);

    /** The ssl configuration during the first connection */
    QSslConfiguration getOrCreateSslConfig();
    QSslConfiguration sslConfiguration() const { return _sslConfiguration; }
    void setSslConfiguration(const QSslConfiguration &config);
    // Because of bugs in Qt, we use this to store info needed for the SSL Button
    QSslCipher _sessionCipher;
    QByteArray _sessionTicket;
    QList<QSslCertificate> _peerCertificateChain;


    /** The certificates of the account */
    QList<QSslCertificate> approvedCerts() const { return _approvedCerts; }
    void setApprovedCerts(const QList<QSslCertificate> certs);
    void addApprovedCerts(const QList<QSslCertificate> certs);

    // Usually when a user explicitly rejects a certificate we don't
    // ask again. After this call, a dialog will again be shown when
    // the next unknown certificate is encountered.
    void resetRejectedCertificates();

    // pluggable handler
    void setSslErrorHandler(AbstractSslErrorHandler *handler);

    // To be called by credentials only, for storing username and the like
    QVariant credentialSetting(const QString &key) const;
    void setCredentialSetting(const QString &key, const QVariant &value);

    /** Assign a client certificate */
    void setCertificate(const QByteArray certficate = QByteArray(), const QString privateKey = QString());

    /** Access the server capabilities */
    const Capabilities &capabilities() const;
    void setCapabilities(const QVariantMap &caps);

    /** Access the server version
     *
     * For servers >= 10.0.0, this can be the empty string until capabilities
     * have been received.
     */
    QString serverVersion() const;

    /** Server version for easy comparison.
     *
     * Example: serverVersionInt() >= makeServerVersion(11, 2, 3)
     *
     * Will be 0 if the version is not available yet.
     */
    int serverVersionInt() const;

    static int makeServerVersion(int majorVersion, int minorVersion, int patchVersion);
    void setServerVersion(const QString &version);

    /** Whether the server is too old.
     *
     * Not supporting server versions is a gradual process. There's a hard
     * compatibility limit (see ConnectionValidator) that forbids connecting
     * to extremely old servers. And there's a weak "untested, not
     * recommended, potentially dangerous" limit, that users might want
     * to go beyond.
     *
     * This function returns true if the server is beyond the weak limit.
     */
    bool serverVersionUnsupported() const;

    /** True when the server connection is using HTTP2  */
    bool isHttp2Supported() { return _http2Supported; }
    void setHttp2Supported(bool value) { _http2Supported = value; }

    void clearCookieJar();
    void lendCookieJarTo(QNetworkAccessManager *guest);
    QString cookieJarPath();

    void resetNetworkAccessManager();
    QNetworkAccessManager *networkAccessManager();
    QSharedPointer<QNetworkAccessManager> sharedNetworkAccessManager();

    /// Called by network jobs on credential errors, emits invalidCredentials()
    void handleInvalidCredentials();

    ClientSideEncryption* e2e();

    /// Used in RemoteWipe
    void retrieveAppPassword();
    void writeAppPasswordOnce(QString appPassword);
    void deleteAppPassword();

    /// Direct Editing
    // Check for the directEditing capability
    void fetchDirectEditors(const QUrl &directEditingURL, const QString &directEditingETag);

    void trySetupPushNotifications();
    PushNotifications *pushNotifications() const;
    void setPushNotificationsReconnectInterval(int interval);

    JobQueue *jobQueue();

public slots:
    /// Used when forgetting credentials
    void clearQNAMCache();
    void slotHandleSslErrors(QNetworkReply *, QList<QSslError>);

signals:
    /// Emitted whenever there's network activity
    void propagatorNetworkActivity();

    /// Triggered by handleInvalidCredentials()
    void invalidCredentials();

    void credentialsFetched(AbstractCredentials *credentials);
    void credentialsAsked(AbstractCredentials *credentials);

    /// Forwards from QNetworkAccessManager::proxyAuthenticationRequired().
    void proxyAuthenticationRequired(const QNetworkProxy &, QAuthenticator *);

    // e.g. when the approved SSL certificates changed
    void wantsAccountSaved(Account *acc);

    void serverVersionChanged(Account *account, const QString &newVersion, const QString &oldVersion);

    void accountChangedAvatar();
    void accountChangedDisplayName();

    /// Used in RemoteWipe
    void appPasswordRetrieved(QString);

    void pushNotificationsReady(Account *account);
    void pushNotificationsDisabled(Account *account);

protected Q_SLOTS:
    void slotCredentialsFetched();
    void slotCredentialsAsked();
    void slotDirectEditingRecieved(const QJsonDocument &json);

private:
    Account(QObject *parent = nullptr);
    void setSharedThis(AccountPtr sharedThis);

    QWeakPointer<Account> _sharedThis;
    QString _id;
    QString _davUser;
    QString _displayName;
    QTimer _pushNotificationsReconnectTimer;
#ifndef TOKEN_AUTH_ONLY
    QImage _avatarImg;
#endif
    QMap<QString, QVariant> _settingsMap;
    QUrl _url;

    /** If url to use for any user-visible urls.
     *
     * If the server configures overwritehost this can be different from
     * the connection url in _url. We retrieve the visible host through
     * the ocs/v1.php/config endpoint in ConnectionValidator.
     */
    QUrl _userVisibleUrl;

    QList<QSslCertificate> _approvedCerts;
    QSslConfiguration _sslConfiguration;
    Capabilities _capabilities;
    QString _serverVersion;
    QScopedPointer<AbstractSslErrorHandler> _sslErrorHandler;
    QSharedPointer<QNetworkAccessManager> _am;
    QScopedPointer<AbstractCredentials> _credentials;
    bool _http2Supported = false;

    /// Certificates that were explicitly rejected by the user
    QList<QSslCertificate> _rejectedCertificates;

    static QString _configFileName;

    QString _davPath; // defaults to value from theme, might be overwritten in brandings

    ClientSideEncryption _e2e;

    /// Used in RemoteWipe
    bool _wroteAppPassword = false;

    JobQueue _jobQueue;

    friend class AccountManager;

    // Direct Editing
    QString _lastDirectEditingETag;

    PushNotifications *_pushNotifications = nullptr;

    /* IMPORTANT - remove later - FIXME MS@2019-12-07 -->
     * TODO: For "Log out" & "Remove account": Remove client CA certs and KEY!
     *
     *       Disabled as long as selecting another cert is not supported by the UI.
     *
     *       Being able to specify a new certificate is important anyway: expiry etc.
     *
     *       We introduce this dirty hack here, to allow deleting them upon Remote Wipe.
    */
    public:
        void setRemoteWipeRequested_HACK() { _isRemoteWipeRequested_HACK = true; }
        bool isRemoteWipeRequested_HACK() { return _isRemoteWipeRequested_HACK; }
    private:
        bool _isRemoteWipeRequested_HACK = false;
    // <-- FIXME MS@2019-12-07
};
}

Q_DECLARE_METATYPE(OCC::AccountPtr)
Q_DECLARE_METATYPE(OCC::Account *)

#endif //SERVERCONNECTION_H
