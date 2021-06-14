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

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QNetworkReply>
#include <QNetworkProxyFactory>
#include <QXmlStreamReader>

#include "connectionvalidator.h"
#include "account.h"
#include "accountstate.h"
#include "userinfo.h"
#include "networkjobs.h"
#include "clientproxy.h"
#include <creds/abstractcredentials.h>

namespace OCC {

Q_LOGGING_CATEGORY(lcConnectionValidator, "nextcloud.sync.connectionvalidator", QtInfoMsg)

// Make sure the timeout for this job is less than how often we get called
// This makes sure we get tried often enough without "ConnectionValidator already running"
static qint64 timeoutToUseMsec = qMax(1000, ConnectionValidator::DefaultCallingIntervalMsec - 5 * 1000);

ConnectionValidator::ConnectionValidator(AccountStatePtr accountState, QObject *parent)
    : QObject(parent)
    , _accountState(accountState)
    , _account(accountState->account())
    , _isCheckingServerAndAuth(false)
{
}

void ConnectionValidator::checkServerAndAuth()
{
    if (!_account) {
        _errors << tr("No Nextcloud account configured");
        reportResult(NotConfigured);
        return;
    }
    qCDebug(lcConnectionValidator) << "Checking server and authentication";

    _isCheckingServerAndAuth = true;

    // Lookup system proxy in a thread https://github.com/owncloud/client/issues/2993
    if (ClientProxy::isUsingSystemDefault()) {
        qCDebug(lcConnectionValidator) << "Trying to look up system proxy";
        ClientProxy::lookupSystemProxyAsync(_account->url(),
            this, SLOT(systemProxyLookupDone(QNetworkProxy)));
    } else {
        // We want to reset the QNAM proxy so that the global proxy settings are used (via ClientProxy settings)
        _account->networkAccessManager()->setProxy(QNetworkProxy(QNetworkProxy::DefaultProxy));
        // use a queued invocation so we're as asynchronous as with the other code path
        QMetaObject::invokeMethod(this, "slotCheckServerAndAuth", Qt::QueuedConnection);
    }
}

void ConnectionValidator::systemProxyLookupDone(const QNetworkProxy &proxy)
{
    if (!_account) {
        qCWarning(lcConnectionValidator) << "Bailing out, Account had been deleted";
        return;
    }

    if (proxy.type() != QNetworkProxy::NoProxy) {
        qCInfo(lcConnectionValidator) << "Setting QNAM proxy to be system proxy" << ClientProxy::printQNetworkProxy(proxy);
    } else {
        qCInfo(lcConnectionValidator) << "No system proxy set by OS";
    }
    _account->networkAccessManager()->setProxy(proxy);

    slotCheckServerAndAuth();
}

// The actual check
void ConnectionValidator::slotCheckServerAndAuth()
{
    auto *checkJob = new CheckServerJob(_account, this);
    checkJob->setTimeout(timeoutToUseMsec);
    checkJob->setIgnoreCredentialFailure(true);
    connect(checkJob, &CheckServerJob::instanceFound, this, &ConnectionValidator::slotStatusFound);
    connect(checkJob, &CheckServerJob::instanceNotFound, this, &ConnectionValidator::slotNoStatusFound);
    connect(checkJob, &CheckServerJob::timeout, this, &ConnectionValidator::slotJobTimeout);
    checkJob->start();
}

void ConnectionValidator::slotStatusFound(const QUrl &url, const QJsonObject &info)
{
    // Newer servers don't disclose any version in status.php anymore
    // https://github.com/owncloud/core/pull/27473/files
    // so this string can be empty.
    QString serverVersion = CheckServerJob::version(info);

    // status.php was found.
    qCInfo(lcConnectionValidator) << "** Application: ownCloud found: "
                                  << url << " with version "
                                  << CheckServerJob::versionString(info)
                                  << "(" << serverVersion << ")";

    // Update server url in case of redirection
    if (_account->url() != url) {
        qCInfo(lcConnectionValidator()) << "status.php was redirected to" << url.toString();
        _account->setUrl(url);
        _account->wantsAccountSaved(_account.data());
    }

    if (!serverVersion.isEmpty() && !setAndCheckServerVersion(serverVersion)) {
        return;
    }

    // Check for maintenance mode: Servers send "true", so go through QVariant
    // to parse it correctly.
    if (info["maintenance"].toVariant().toBool()) {
        reportResult(MaintenanceMode);
        return;
    }

    // now check the authentication
    QTimer::singleShot(0, this, &ConnectionValidator::checkAuthentication);
}

// status.php could not be loaded (network or server issue!).
void ConnectionValidator::slotNoStatusFound(QNetworkReply *reply)
{
    auto job = qobject_cast<CheckServerJob *>(sender());
    qCWarning(lcConnectionValidator) << reply->error() << job->errorString() << reply->peek(1024);
    if (reply->error() == QNetworkReply::SslHandshakeFailedError) {
        reportResult(SslError);
        return;
    }

    if (!_account->credentials()->stillValid(reply)) {
        // Note: Why would this happen on a status.php request?
        _errors.append(tr("Authentication error: Either username or password are wrong."));
    } else {
        //_errors.append(tr("Unable to connect to %1").arg(_account->url().toString()));
        _errors.append(job->errorString());
    }
    reportResult(StatusNotFound);
}

void ConnectionValidator::slotJobTimeout(const QUrl &url)
{
    Q_UNUSED(url);
    //_errors.append(tr("Unable to connect to %1").arg(url.toString()));
    _errors.append(tr("Timeout"));
    reportResult(Timeout);
}


void ConnectionValidator::checkAuthentication()
{
    AbstractCredentials *creds = _account->credentials();

    if (!creds->ready()) {
        reportResult(CredentialsNotReady);
        return;
    }

    // simply GET the webdav root, will fail if credentials are wrong.
    // continue in slotAuthCheck here :-)
    qCDebug(lcConnectionValidator) << "# Check whether authenticated propfind works.";
    auto *job = new PropfindJob(_account, "/", this);
    job->setTimeout(timeoutToUseMsec);
    job->setProperties(QList<QByteArray>() << "getlastmodified");
    connect(job, &PropfindJob::result, this, &ConnectionValidator::slotAuthSuccess);
    connect(job, &PropfindJob::finishedWithError, this, &ConnectionValidator::slotAuthFailed);
    job->start();
}

void ConnectionValidator::slotAuthFailed(QNetworkReply *reply)
{
    auto job = qobject_cast<PropfindJob *>(sender());
    Status stat = Timeout;

    if (reply->error() == QNetworkReply::SslHandshakeFailedError) {
        _errors << job->errorStringParsingBody();
        stat = SslError;

    } else if (reply->error() == QNetworkReply::AuthenticationRequiredError
        || !_account->credentials()->stillValid(reply)) {
        auto authChallenge = reply->rawHeader("WWW-Authenticate").toLower();
        qCWarning(lcConnectionValidator) << "******** Password is wrong!" << reply->error() << job->errorString() << authChallenge;
        _errors << tr("The provided credentials are not correct");
        stat = CredentialsWrong;

    } else if (reply->error() != QNetworkReply::NoError) {
        _errors << job->errorStringParsingBody();

        const int httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (httpStatus == 503) {
            _errors.clear();
            stat = ServiceUnavailable;
        }
    }

    reportResult(stat);
}

void ConnectionValidator::slotAuthSuccess()
{
    _errors.clear();
    if (!_isCheckingServerAndAuth) {
        reportResult(Connected);
        return;
    }
    checkServerCapabilities();
}

void ConnectionValidator::checkServerCapabilities()
{
    // The main flow now needs the capabilities
    auto *job = new JsonApiJob(_account, QLatin1String("ocs/v1.php/cloud/capabilities"), this);
    job->setTimeout(timeoutToUseMsec);
    QObject::connect(job, &JsonApiJob::jsonReceived, this, &ConnectionValidator::slotCapabilitiesRecieved);
    job->start();

    // And we'll retrieve the ocs config in parallel
    // note that 'this' might be destroyed before the job finishes, so intentionally not parented
    auto configJob = new JsonApiJob(_account, QLatin1String("ocs/v1.php/config"));
    configJob->setTimeout(timeoutToUseMsec);
    auto account = _account; // capturing account by value will make it live long enough
    QObject::connect(configJob, &JsonApiJob::jsonReceived, _account.data(),
        [=](const QJsonDocument &json) {
            ocsConfigReceived(json, account);
        });
    configJob->start();
}

void ConnectionValidator::slotCapabilitiesRecieved(const QJsonDocument &json)
{
    auto caps = json.object().value("ocs").toObject().value("data").toObject().value("capabilities").toObject();
    qCInfo(lcConnectionValidator) << "Server capabilities" << caps;
    _account->setCapabilities(caps.toVariantMap());

    // New servers also report the version in the capabilities
    QString serverVersion = caps["core"].toObject()["status"].toObject()["version"].toString();
    if (!serverVersion.isEmpty() && !setAndCheckServerVersion(serverVersion)) {
        return;
    }

    // Check for the directEditing capability
    QUrl directEditingURL = QUrl(caps["files"].toObject()["directEditing"].toObject()["url"].toString());
    QString directEditingETag = caps["files"].toObject()["directEditing"].toObject()["etag"].toString();
    _account->fetchDirectEditors(directEditingURL, directEditingETag);

    fetchUser();
}

void ConnectionValidator::ocsConfigReceived(const QJsonDocument &json, AccountPtr account)
{
    QString host = json.object().value("ocs").toObject().value("data").toObject().value("host").toString();
    if (host.isEmpty()) {
        qCWarning(lcConnectionValidator) << "Could not extract 'host' from ocs config reply";
        return;
    }
    qCInfo(lcConnectionValidator) << "Determined user-visible host to be" << host;
    account->setUserVisibleHost(host);
}

void ConnectionValidator::fetchUser()
{
    auto *userInfo = new UserInfo(_accountState.data(), true, true, this);
    QObject::connect(userInfo, &UserInfo::fetchedLastInfo, this, &ConnectionValidator::slotUserFetched);
    userInfo->setActive(true);
}

bool ConnectionValidator::setAndCheckServerVersion(const QString &version)
{
    qCInfo(lcConnectionValidator) << _account->url() << "has server version" << version;
    _account->setServerVersion(version);

    // We cannot deal with servers < 7.0.0
    if (_account->serverVersionInt()
        && _account->serverVersionInt() < Account::makeServerVersion(7, 0, 0)) {
        _errors.append(tr("The configured server for this client is too old"));
        _errors.append(tr("Please update to the latest server and restart the client."));
        reportResult(ServerVersionMismatch);
        return false;
    }
    // We attempt to work with servers >= 7.0.0 but warn users.
    // Check usages of Account::serverVersionUnsupported() for details.

#if QT_VERSION >= QT_VERSION_CHECK(5, 9, 0)
    // Record that the server supports HTTP/2
    // Actual decision if we should use HTTP/2 is done in AccessManager::createRequest
    if (auto job = qobject_cast<AbstractNetworkJob *>(sender())) {
        if (auto reply = job->reply()) {
            _account->setHttp2Supported(
                reply->attribute(QNetworkRequest::HTTP2WasUsedAttribute).toBool());
        }
    }
#endif
    return true;
}

void ConnectionValidator::slotUserFetched(UserInfo *userInfo)
{
    if(userInfo) {
        userInfo->setActive(false);
        userInfo->deleteLater();
    }

#ifndef TOKEN_AUTH_ONLY
    connect(_account->e2e(), &ClientSideEncryption::initializationFinished, this, &ConnectionValidator::reportConnected);
    _account->e2e()->initialize(_account);
#else
    reportResult(Connected);
#endif
}

#ifndef TOKEN_AUTH_ONLY
void ConnectionValidator::reportConnected() {
    reportResult(Connected);
}
#endif

void ConnectionValidator::reportResult(Status status)
{
    emit connectionResult(status, _errors);
    deleteLater();
}

} // namespace OCC
