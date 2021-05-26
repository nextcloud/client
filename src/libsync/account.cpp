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

#include "account.h"
#include "cookiejar.h"
#include "networkjobs.h"
#include "configfile.h"
#include "accessmanager.h"
#include "creds/abstractcredentials.h"
#include "capabilities.h"
#include "theme.h"
#include "pushnotifications.h"
#include "version.h"

#include "common/asserts.h"
#include "clientsideencryption.h"

#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkAccessManager>
#include <QSslSocket>
#include <QNetworkCookieJar>
#include <QNetworkProxy>

#include <QFileInfo>
#include <QDir>
#include <QSslKey>
#include <QAuthenticator>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <qt5keychain/keychain.h>
#include "creds/abstractcredentials.h"

using namespace QKeychain;

namespace {
constexpr int pushNotificationsReconnectInterval = 1000 * 60 * 2;
}

namespace OCC {

Q_LOGGING_CATEGORY(lcAccount, "nextcloud.sync.account", QtInfoMsg)
const char app_password[] = "_app-password";

Account::Account(QObject *parent)
    : QObject(parent)
    , _capabilities(QVariantMap())
    , _davPath(Theme::instance()->webDavPath())
{
    qRegisterMetaType<AccountPtr>("AccountPtr");
    qRegisterMetaType<Account *>("Account*");

    _pushNotificationsReconnectTimer.setInterval(pushNotificationsReconnectInterval);
    connect(&_pushNotificationsReconnectTimer, &QTimer::timeout, this, &Account::trySetupPushNotifications);
}

AccountPtr Account::create()
{
    AccountPtr acc = AccountPtr(new Account);
    acc->setSharedThis(acc);
    return acc;
}

ClientSideEncryption* Account::e2e()
{
    // Qt expects everything in the connect to be a pointer, so return a pointer.
    return &_e2e;
}

Account::~Account() = default;

QString Account::davPath() const
{
    if (capabilities().chunkingNg()) {
        // The chunking-ng means the server prefer to use the new webdav URL
        return QLatin1String("/remote.php/dav/files/") + davUser() + QLatin1Char('/');
    }

    // make sure to have a trailing slash
    if (!_davPath.endsWith('/')) {
        QString dp(_davPath);
        dp.append('/');
        return dp;
    }
    return _davPath;
}

void Account::setSharedThis(AccountPtr sharedThis)
{
    _sharedThis = sharedThis.toWeakRef();
}

AccountPtr Account::sharedFromThis()
{
    return _sharedThis.toStrongRef();
}

QString Account::davUser() const
{
    return _davUser.isEmpty() ? _credentials->user() : _davUser;
}

void Account::setDavUser(const QString &newDavUser)
{
    if (_davUser == newDavUser)
        return;
    _davUser = newDavUser;
    emit wantsAccountSaved(this);
}

#ifndef TOKEN_AUTH_ONLY
QImage Account::avatar() const
{
    return _avatarImg;
}
void Account::setAvatar(const QImage &img)
{
    _avatarImg = img;
    emit accountChangedAvatar();
}
#endif

QString Account::displayName() const
{
    QString dn = QString("%1@%2").arg(credentials()->user(), _url.host());
    int port = url().port();
    if (port > 0 && port != 80 && port != 443) {
        dn.append(QLatin1Char(':'));
        dn.append(QString::number(port));
    }
    return dn;
}

QString Account::davDisplayName() const
{
    return _displayName;
}

void Account::setDavDisplayName(const QString &newDisplayName)
{
    _displayName = newDisplayName;
    emit accountChangedDisplayName();
}

QString Account::id() const
{
    return _id;
}

AbstractCredentials *Account::credentials() const
{
    return _credentials.data();
}

void Account::setCredentials(AbstractCredentials *cred)
{
    // set active credential manager
    QNetworkCookieJar *jar = nullptr;
    QNetworkProxy proxy;

    if (_am) {
        jar = _am->cookieJar();
        jar->setParent(nullptr);

        // Remember proxy (issue #2108)
        proxy = _am->proxy();

        _am = QSharedPointer<QNetworkAccessManager>();
    }

    // The order for these two is important! Reading the credential's
    // settings accesses the account as well as account->_credentials,
    _credentials.reset(cred);
    cred->setAccount(this);

    // Note: This way the QNAM can outlive the Account and Credentials.
    // This is necessary to avoid issues with the QNAM being deleted while
    // processing slotHandleSslErrors().
    _am = QSharedPointer<QNetworkAccessManager>(_credentials->createQNAM(), &QObject::deleteLater);

    if (jar) {
        _am->setCookieJar(jar);
    }
    if (proxy.type() != QNetworkProxy::DefaultProxy) {
        _am->setProxy(proxy);
    }
    connect(_am.data(), SIGNAL(sslErrors(QNetworkReply *, QList<QSslError>)),
        SLOT(slotHandleSslErrors(QNetworkReply *, QList<QSslError>)));
    connect(_am.data(), &QNetworkAccessManager::proxyAuthenticationRequired,
        this, &Account::proxyAuthenticationRequired);
    connect(_credentials.data(), &AbstractCredentials::fetched,
        this, &Account::slotCredentialsFetched);
    connect(_credentials.data(), &AbstractCredentials::asked,
        this, &Account::slotCredentialsAsked);

    trySetupPushNotifications();
}

void Account::setPushNotificationsReconnectInterval(int interval)
{
    _pushNotificationsReconnectTimer.setInterval(interval);
}

void Account::trySetupPushNotifications()
{
    // Stop the timer to prevent parallel setup attempts
    _pushNotificationsReconnectTimer.stop();

    if (_capabilities.availablePushNotifications() != PushNotificationType::None) {
        qCInfo(lcAccount) << "Try to setup push notifications";

        if (!_pushNotifications) {
            _pushNotifications = new PushNotifications(this, this);

            connect(_pushNotifications, &PushNotifications::ready, this, [this]() {
                _pushNotificationsReconnectTimer.stop();
                emit pushNotificationsReady(this);
            });

            const auto disablePushNotifications = [this]() {
                qCInfo(lcAccount) << "Disable push notifications object because authentication failed or connection lost";
                if (!_pushNotifications) {
                    return;
                }
                if (!_pushNotifications->isReady()) {
                    emit pushNotificationsDisabled(this);
                }
                if (!_pushNotificationsReconnectTimer.isActive()) {
                    _pushNotificationsReconnectTimer.start();
                }
            };

            connect(_pushNotifications, &PushNotifications::connectionLost, this, disablePushNotifications);
            connect(_pushNotifications, &PushNotifications::authenticationFailed, this, disablePushNotifications);
        }
        // If push notifications already running it is no problem to call setup again
        _pushNotifications->setup();
    }
}

QUrl Account::davUrl() const
{
    return Utility::concatUrlPath(url(), davPath());
}

QUrl Account::deprecatedPrivateLinkUrl(const QByteArray &numericFileId) const
{
    return Utility::concatUrlPath(_userVisibleUrl,
        QLatin1String("/index.php/f/") + QUrl::toPercentEncoding(QString::fromLatin1(numericFileId)));
}

/**
 * clear all cookies. (Session cookies or not)
 */
void Account::clearCookieJar()
{
    auto jar = qobject_cast<CookieJar *>(_am->cookieJar());
    ASSERT(jar);
    jar->setAllCookies(QList<QNetworkCookie>());
    emit wantsAccountSaved(this);
}

/*! This shares our official cookie jar (containing all the tasty
    authentication cookies) with another QNAM while making sure
    of not losing its ownership. */
void Account::lendCookieJarTo(QNetworkAccessManager *guest)
{
    auto jar = _am->cookieJar();
    auto oldParent = jar->parent();
    guest->setCookieJar(jar); // takes ownership of our precious cookie jar
    jar->setParent(oldParent); // takes it back
}

QString Account::cookieJarPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/" + Theme::instance()->appRevDomain() + "/cookies" + id() + ".db";
}

void Account::resetNetworkAccessManager()
{
    if (!_credentials || !_am) {
        return;
    }

    qCDebug(lcAccount) << "Resetting QNAM";
    QNetworkCookieJar *jar = _am->cookieJar();
    QNetworkProxy proxy = _am->proxy();

    // Use a QSharedPointer to allow locking the life of the QNAM on the stack.
    // Make it call deleteLater to make sure that we can return to any QNAM stack frames safely.
    _am = QSharedPointer<QNetworkAccessManager>(_credentials->createQNAM(), &QObject::deleteLater);

    _am->setCookieJar(jar); // takes ownership of the old cookie jar
    _am->setProxy(proxy);   // Remember proxy (issue #2108)

    connect(_am.data(), SIGNAL(sslErrors(QNetworkReply *, QList<QSslError>)),
        SLOT(slotHandleSslErrors(QNetworkReply *, QList<QSslError>)));
    connect(_am.data(), &QNetworkAccessManager::proxyAuthenticationRequired,
        this, &Account::proxyAuthenticationRequired);
}

QNetworkAccessManager *Account::networkAccessManager()
{
    return _am.data();
}

QSharedPointer<QNetworkAccessManager> Account::sharedNetworkAccessManager()
{
    return _am;
}

QNetworkReply *Account::sendRawRequest(const QByteArray &verb, const QUrl &url, QNetworkRequest req, QIODevice *data)
{
    req.setUrl(url);
    req.setSslConfiguration(this->getOrCreateSslConfig());
    if (verb == "HEAD" && !data) {
        return _am->head(req);
    } else if (verb == "GET" && !data) {
        return _am->get(req);
    } else if (verb == "POST") {
        return _am->post(req, data);
    } else if (verb == "PUT") {
        return _am->put(req, data);
    } else if (verb == "DELETE" && !data) {
        return _am->deleteResource(req);
    }
    return _am->sendCustomRequest(req, verb, data);
}

SimpleNetworkJob *Account::sendRequest(const QByteArray &verb, const QUrl &url, QNetworkRequest req, QIODevice *data)
{
    auto job = new SimpleNetworkJob(sharedFromThis());
    job->startRequest(verb, url, req, data);
    return job;
}

void Account::setSslConfiguration(const QSslConfiguration &config)
{
    _sslConfiguration = config;
}

QSslConfiguration Account::getOrCreateSslConfig()
{
    if (!_sslConfiguration.isNull()) {
        // Will be set by CheckServerJob::finished()
        // We need to use a central shared config to get SSL session tickets
        return _sslConfiguration;
    }

    // if setting the client certificate fails, you will probably get an error similar to this:
    //  "An internal error number 1060 happened. SSL handshake failed, client certificate was requested: SSL error: sslv3 alert handshake failure"
    QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();

    // Try hard to re-use session for different requests
    sslConfig.setSslOption(QSsl::SslOptionDisableSessionTickets, false);
    sslConfig.setSslOption(QSsl::SslOptionDisableSessionSharing, false);
    sslConfig.setSslOption(QSsl::SslOptionDisableSessionPersistence, false);

    return sslConfig;
}

void Account::setApprovedCerts(const QList<QSslCertificate> certs)
{
    _approvedCerts = certs;
    QSslSocket::addDefaultCaCertificates(certs);
}

void Account::addApprovedCerts(const QList<QSslCertificate> certs)
{
    _approvedCerts += certs;
}

void Account::resetRejectedCertificates()
{
    _rejectedCertificates.clear();
}

void Account::setSslErrorHandler(AbstractSslErrorHandler *handler)
{
    _sslErrorHandler.reset(handler);
}

void Account::setUrl(const QUrl &url)
{
    _url = url;
    _userVisibleUrl = url;
}

void Account::setUserVisibleHost(const QString &host)
{
    _userVisibleUrl.setHost(host);
}

QVariant Account::credentialSetting(const QString &key) const
{
    if (_credentials) {
        QString prefix = _credentials->authType();
        QVariant value = _settingsMap.value(prefix + "_" + key);
        if (value.isNull()) {
            value = _settingsMap.value(key);
        }
        return value;
    }
    return QVariant();
}

void Account::setCredentialSetting(const QString &key, const QVariant &value)
{
    if (_credentials) {
        QString prefix = _credentials->authType();
        _settingsMap.insert(prefix + "_" + key, value);
    }
}

void Account::slotHandleSslErrors(QNetworkReply *reply, QList<QSslError> errors)
{
    NetworkJobTimeoutPauser pauser(reply);
    QString out;
    QDebug(&out) << "SSL-Errors happened for url " << reply->url().toString();
    foreach (const QSslError &error, errors) {
        QDebug(&out) << "\tError in " << error.certificate() << ":"
                     << error.errorString() << "(" << error.error() << ")"
                     << "\n";
    }

    bool allPreviouslyRejected = true;
    foreach (const QSslError &error, errors) {
        if (!_rejectedCertificates.contains(error.certificate())) {
            allPreviouslyRejected = false;
        }
    }

    // If all certs have previously been rejected by the user, don't ask again.
    if (allPreviouslyRejected) {
        qCInfo(lcAccount) << out << "Certs not trusted by user decision, returning.";
        return;
    }

    QList<QSslCertificate> approvedCerts;
    if (_sslErrorHandler.isNull()) {
        qCWarning(lcAccount) << out << "called without valid SSL error handler for account" << url();
        return;
    }

    // SslDialogErrorHandler::handleErrors will run an event loop that might execute
    // the deleteLater() of the QNAM before we have the chance of unwinding our stack.
    // Keep a ref here on our stackframe to make sure that it doesn't get deleted before
    // handleErrors returns.
    QSharedPointer<QNetworkAccessManager> qnamLock = _am;
    QPointer<QObject> guard = reply;

    if (_sslErrorHandler->handleErrors(errors, reply->sslConfiguration(), &approvedCerts, sharedFromThis())) {
        if (!guard)
            return;

        if (!approvedCerts.isEmpty()) {
            QSslSocket::addDefaultCaCertificates(approvedCerts);
            addApprovedCerts(approvedCerts);
            emit wantsAccountSaved(this);

            // all ssl certs are known and accepted. We can ignore the problems right away.
            qCInfo(lcAccount) << out << "Certs are known and trusted! This is not an actual error.";
        }

        // Warning: Do *not* use ignoreSslErrors() (without args) here:
        // it permanently ignores all SSL errors for this host, even
        // certificate changes.
        reply->ignoreSslErrors(errors);
    } else {
        if (!guard)
            return;

        // Mark all involved certificates as rejected, so we don't ask the user again.
        foreach (const QSslError &error, errors) {
            if (!_rejectedCertificates.contains(error.certificate())) {
                _rejectedCertificates.append(error.certificate());
            }
        }

        // Not calling ignoreSslErrors will make the SSL handshake fail.
        return;
    }
}

void Account::slotCredentialsFetched()
{
    emit credentialsFetched(_credentials.data());
}

void Account::slotCredentialsAsked()
{
    emit credentialsAsked(_credentials.data());
}

void Account::handleInvalidCredentials()
{
    // Retrieving password will trigger remote wipe check job
    retrieveAppPassword();

    emit invalidCredentials();
}

void Account::clearQNAMCache()
{
    _am->clearAccessCache();
}

const Capabilities &Account::capabilities() const
{
    return _capabilities;
}

void Account::setCapabilities(const QVariantMap &caps)
{
    _capabilities = Capabilities(caps);

    trySetupPushNotifications();
}

QString Account::serverVersion() const
{
    return _serverVersion;
}

int Account::serverVersionInt() const
{
    // FIXME: Use Qt 5.5 QVersionNumber
    auto components = serverVersion().split('.');
    return makeServerVersion(components.value(0).toInt(),
        components.value(1).toInt(),
        components.value(2).toInt());
}

int Account::makeServerVersion(int majorVersion, int minorVersion, int patchVersion)
{
    return (majorVersion << 16) + (minorVersion << 8) + patchVersion;
}

bool Account::serverVersionUnsupported() const
{
    if (serverVersionInt() == 0) {
        // not detected yet, assume it is fine.
        return false;
    }
    return serverVersionInt() < makeServerVersion(NEXTCLOUD_SERVER_VERSION_MIN_SUPPORTED_MAJOR,
               NEXTCLOUD_SERVER_VERSION_MIN_SUPPORTED_MINOR, NEXTCLOUD_SERVER_VERSION_MIN_SUPPORTED_PATCH);
}

void Account::setServerVersion(const QString &version)
{
    if (version == _serverVersion) {
        return;
    }

    auto oldServerVersion = _serverVersion;
    _serverVersion = version;
    emit serverVersionChanged(this, oldServerVersion, version);
}

void Account::setNonShib(bool nonShib)
{
    if (nonShib) {
        _davPath = Theme::instance()->webDavPathNonShib();
    } else {
        _davPath = Theme::instance()->webDavPath();
    }
}

void Account::writeAppPasswordOnce(QString appPassword){
    if(_wroteAppPassword)
        return;

    // Fix: Password got written from Account Wizard, before finish.
    // Only write the app password for a connected account, else
    // there'll be a zombie keychain slot forever, never used again ;p
    //
    // Also don't write empty passwords (Log out -> Relaunch)
    if(id().isEmpty() || appPassword.isEmpty())
        return;

    const QString kck = AbstractCredentials::keychainKey(
                url().toString(),
                davUser() + app_password,
                id()
    );

    auto *job = new WritePasswordJob(Theme::instance()->appName());
    job->setInsecureFallback(false);
    job->setKey(kck);
    job->setBinaryData(appPassword.toLatin1());
    connect(job, &WritePasswordJob::finished, [this](Job *incoming) {
        auto *writeJob = static_cast<WritePasswordJob *>(incoming);
        if (writeJob->error() == NoError)
            qCInfo(lcAccount) << "appPassword stored in keychain";
        else
            qCWarning(lcAccount) << "Unable to store appPassword in keychain" << writeJob->errorString();

        // We don't try this again on error, to not raise CPU consumption
        _wroteAppPassword = true;
    });
    job->start();
}

void Account::retrieveAppPassword(){
    const QString kck = AbstractCredentials::keychainKey(
                url().toString(),
                credentials()->user() + app_password,
                id()
    );

    auto *job = new ReadPasswordJob(Theme::instance()->appName());
    job->setInsecureFallback(false);
    job->setKey(kck);
    connect(job, &ReadPasswordJob::finished, [this](Job *incoming) {
        auto *readJob = static_cast<ReadPasswordJob *>(incoming);
        QString pwd("");
        // Error or no valid public key error out
        if (readJob->error() == NoError &&
                readJob->binaryData().length() > 0) {
            pwd = readJob->binaryData();
        }

        emit appPasswordRetrieved(pwd);
    });
    job->start();
}

void Account::deleteAppPassword(){
    const QString kck = AbstractCredentials::keychainKey(
                url().toString(),
                credentials()->user() + app_password,
                id()
    );

    if (kck.isEmpty()) {
        qCDebug(lcAccount) << "appPassword is empty";
        return;
    }

    auto *job = new DeletePasswordJob(Theme::instance()->appName());
    job->setInsecureFallback(false);
    job->setKey(kck);
    connect(job, &DeletePasswordJob::finished, [this](Job *incoming) {
        auto *deleteJob = static_cast<DeletePasswordJob *>(incoming);
        if (deleteJob->error() == NoError)
            qCInfo(lcAccount) << "appPassword deleted from keychain";
        else
            qCWarning(lcAccount) << "Unable to delete appPassword from keychain" << deleteJob->errorString();

        // Allow storing a new app password on re-login
        _wroteAppPassword = false;
    });
    job->start();
}

void Account::fetchDirectEditors(const QUrl &directEditingURL, const QString &directEditingETag)
{
    if(directEditingURL.isEmpty() || directEditingETag.isEmpty())
        return;

    // Check for the directEditing capability
    if (!directEditingURL.isEmpty() &&
        (directEditingETag.isEmpty() || directEditingETag != _lastDirectEditingETag)) {
            // Fetch the available editors and their mime types
            auto *job = new JsonApiJob(sharedFromThis(), QLatin1String("ocs/v2.php/apps/files/api/v1/directEditing"));
            QObject::connect(job, &JsonApiJob::jsonReceived, this, &Account::slotDirectEditingRecieved);
            job->start();
    }
}

void Account::slotDirectEditingRecieved(const QJsonDocument &json)
{
    auto data = json.object().value("ocs").toObject().value("data").toObject();
    auto editors = data.value("editors").toObject();

    foreach (auto editorKey, editors.keys()) {
        auto editor = editors.value(editorKey).toObject();

        const QString id = editor.value("id").toString();
        const QString name = editor.value("name").toString();

        if(!id.isEmpty() && !name.isEmpty()) {
            auto mimeTypes = editor.value("mimetypes").toArray();
            auto optionalMimeTypes = editor.value("optionalMimetypes").toArray();

            auto *directEditor = new DirectEditor(id, name);

            foreach(auto mimeType, mimeTypes) {
                directEditor->addMimetype(mimeType.toString().toLatin1());
            }

            foreach(auto optionalMimeType, optionalMimeTypes) {
                directEditor->addOptionalMimetype(optionalMimeType.toString().toLatin1());
            }

            _capabilities.addDirectEditor(directEditor);
        }
    }
}

PushNotifications *Account::pushNotifications() const
{
    return _pushNotifications;
}

} // namespace OCC
