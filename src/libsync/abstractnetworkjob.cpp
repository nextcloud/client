/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#include <QLoggingCategory>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QBuffer>
#include <QXmlStreamReader>
#include <QStringList>
#include <QStack>
#include <QTimer>
#include <QMutex>
#include <QCoreApplication>
#include <QAuthenticator>
#include <QMetaEnum>
#include <QRegularExpression>

#include "common/asserts.h"
#include "networkjobs.h"
#include "account.h"
#include "owncloudpropagator.h"
#include "httplogger.h"

#include "creds/abstractcredentials.h"

Q_DECLARE_METATYPE(QTimer *)

namespace OCC {

Q_LOGGING_CATEGORY(lcNetworkJob, "nextcloud.sync.networkjob", QtInfoMsg)

// If not set, it is overwritten by the Application constructor with the value from the config
int AbstractNetworkJob::httpTimeout = qEnvironmentVariableIntValue("OWNCLOUD_TIMEOUT");

AbstractNetworkJob::AbstractNetworkJob(AccountPtr account, const QString &path, QObject *parent)
    : QObject(parent)
    , _timedout(false)
    , _followRedirects(true)
    , _account(account)
    , _ignoreCredentialFailure(false)
    , _reply(nullptr)
    , _path(path)
{
    // Since we hold a QSharedPointer to the account, this makes no sense. (issue #6893)
    ASSERT(account != parent);

    _timer.setSingleShot(true);
    _timer.setInterval((httpTimeout ? httpTimeout : 300) * 1000); // default to 5 minutes.
    connect(&_timer, &QTimer::timeout, this, &AbstractNetworkJob::slotTimeout);

    connect(this, &AbstractNetworkJob::networkActivity, this, &AbstractNetworkJob::resetTimeout);

    // Network activity on the propagator jobs (GET/PUT) keeps all requests alive.
    // This is a workaround for OC instances which only support one
    // parallel up and download
    if (_account) {
        connect(_account.data(), &Account::propagatorNetworkActivity, this, &AbstractNetworkJob::resetTimeout);
    }
}

void AbstractNetworkJob::setReply(QNetworkReply *reply)
{
    QNetworkReply *old = _reply;
    _reply = reply;
    delete old;
}

void AbstractNetworkJob::setTimeout(qint64 msec)
{
    _timer.start(msec);
}

void AbstractNetworkJob::resetTimeout()
{
    qint64 interval = _timer.interval();
    _timer.stop();
    _timer.start(interval);
}

void AbstractNetworkJob::setIgnoreCredentialFailure(bool ignore)
{
    _ignoreCredentialFailure = ignore;
}

void AbstractNetworkJob::setFollowRedirects(bool follow)
{
    _followRedirects = follow;
}

void AbstractNetworkJob::setPath(const QString &path)
{
    _path = path;
}

void AbstractNetworkJob::setupConnections(QNetworkReply *reply)
{
    connect(reply, &QNetworkReply::finished, this, &AbstractNetworkJob::slotFinished);
    connect(reply, &QNetworkReply::encrypted, this, &AbstractNetworkJob::networkActivity);
    connect(reply->manager(), &QNetworkAccessManager::proxyAuthenticationRequired, this, &AbstractNetworkJob::networkActivity);
    connect(reply, &QNetworkReply::sslErrors, this, &AbstractNetworkJob::networkActivity);
    connect(reply, &QNetworkReply::metaDataChanged, this, &AbstractNetworkJob::networkActivity);
    connect(reply, &QNetworkReply::downloadProgress, this, &AbstractNetworkJob::networkActivity);
    connect(reply, &QNetworkReply::uploadProgress, this, &AbstractNetworkJob::networkActivity);
}

QNetworkReply *AbstractNetworkJob::addTimer(QNetworkReply *reply)
{
    reply->setProperty("timer", QVariant::fromValue(&_timer));
    return reply;
}

bool AbstractNetworkJob::isAuthenticationJob() const
{
    return _isAuthenticationJob;
}

void AbstractNetworkJob::setAuthenticationJob(bool b)
{
    _isAuthenticationJob = b;
}

void AbstractNetworkJob::sendRequest(const QByteArray &verb, const QUrl &url,
    QNetworkRequest req, QIODevice *requestBody)
{
    _verb = verb;
    _request = req;
    _request.setUrl(url);
    _requestBody = requestBody;
    if (!isAuthenticationJob() && _account->jobQueue()->enqueue(this)) {
        return;
    }
    auto reply = _account->sendRawRequest(verb, url, req, requestBody);
    if (_requestBody) {
        _requestBody->setParent(reply);
    }
    adoptRequest(reply);
}

void AbstractNetworkJob::adoptRequest(QNetworkReply *reply)
{
    addTimer(reply);
    setReply(reply);
    setupConnections(reply);
    newReplyHook(reply);
}

QUrl AbstractNetworkJob::makeAccountUrl(const QString &relativePath) const
{
    return Utility::concatUrlPath(_account->url(), relativePath);
}

QUrl AbstractNetworkJob::makeDavUrl(const QString &relativePath) const
{
    return Utility::concatUrlPath(_account->davUrl(), relativePath);
}

void AbstractNetworkJob::slotFinished()
{
    _timer.stop();

    if (_reply->error() == QNetworkReply::SslHandshakeFailedError) {
        qCWarning(lcNetworkJob) << "SslHandshakeFailedError: " << errorString() << " : can be caused by a webserver wanting SSL client certificates";
    }
    // Qt doesn't yet transparently resend HTTP2 requests, do so here
    const auto maxHttp2Resends = 3;
    QByteArray verb = HttpLogger::requestVerb(*reply());
    if (_reply->error() == QNetworkReply::ContentReSendError
        && _reply->attribute(QNetworkRequest::HTTP2WasUsedAttribute).toBool()) {

        if ((_requestBody && !_requestBody->isSequential()) || verb.isEmpty()) {
            qCWarning(lcNetworkJob) << "Can't resend HTTP2 request, verb or body not suitable"
                                    << _reply->request().url() << verb << _requestBody;
        } else if (_http2ResendCount >= maxHttp2Resends) {
            qCWarning(lcNetworkJob) << "Not resending HTTP2 request, number of resends exhausted"
                                    << _reply->request().url() << _http2ResendCount;
        } else {
            qCInfo(lcNetworkJob) << "HTTP2 resending" << _reply->request().url();
            _http2ResendCount++;

            resetTimeout();
            if (_requestBody) {
                if(!_requestBody->isOpen())
                   _requestBody->open(QIODevice::ReadOnly);
                _requestBody->seek(0);
            }
            sendRequest(
                verb,
                _reply->request().url(),
                _reply->request(),
                _requestBody);
            return;
        }
    }

    if (_reply->error() != QNetworkReply::NoError) {
        if (_account->jobQueue()->retry(this)) {
            qCDebug(lcNetworkJob) << "Queuing: " << _reply->url() << " for retry";
            return;
        }

        if (!_ignoreCredentialFailure || _reply->error() != QNetworkReply::AuthenticationRequiredError) {
            qCWarning(lcNetworkJob) << _reply->error() << errorString()
                                    << _reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
            if (_reply->error() == QNetworkReply::ProxyAuthenticationRequiredError) {
                qCWarning(lcNetworkJob) << _reply->rawHeader("Proxy-Authenticate");
            }
        }
        emit networkError(_reply);
    }

    // get the Date timestamp from reply
    _responseTimestamp = _reply->rawHeader("Date");

    QUrl requestedUrl = reply()->request().url();
    QUrl redirectUrl = reply()->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (_followRedirects && !redirectUrl.isEmpty()) {
        // Redirects may be relative
        if (redirectUrl.isRelative())
            redirectUrl = requestedUrl.resolved(redirectUrl);

        // For POST requests where the target url has query arguments, Qt automatically
        // moves these arguments to the body if no explicit body is specified.
        // This can cause problems with redirected requests, because the redirect url
        // will no longer contain these query arguments.
        if (reply()->operation() == QNetworkAccessManager::PostOperation
            && requestedUrl.hasQuery()
            && !redirectUrl.hasQuery()
            && !_requestBody) {
            qCWarning(lcNetworkJob) << "Redirecting a POST request with an implicit body loses that body";
        }

        // ### some of the qWarnings here should be exported via displayErrors() so they
        // ### can be presented to the user if the job executor has a GUI
        if (requestedUrl.scheme() == QLatin1String("https") && redirectUrl.scheme() == QLatin1String("http")) {
            qCWarning(lcNetworkJob) << this << "HTTPS->HTTP downgrade detected!";
        } else if (requestedUrl == redirectUrl || _redirectCount + 1 >= maxRedirects()) {
            qCWarning(lcNetworkJob) << this << "Redirect loop detected!";
        } else if (_requestBody && _requestBody->isSequential()) {
            qCWarning(lcNetworkJob) << this << "cannot redirect request with sequential body";
        } else if (verb.isEmpty()) {
            qCWarning(lcNetworkJob) << this << "cannot redirect request: could not detect original verb";
        } else {
            emit redirected(_reply, redirectUrl, _redirectCount);

            // The signal emission may have changed this value
            if (_followRedirects) {
                _redirectCount++;

                // Create the redirected request and send it
                qCInfo(lcNetworkJob) << "Redirecting" << verb << requestedUrl << redirectUrl;
                resetTimeout();
                if (_requestBody) {
                    if(!_requestBody->isOpen()) {
                        // Avoid the QIODevice::seek (QBuffer): The device is not open warning message
                       _requestBody->open(QIODevice::ReadOnly);
                    }
                    _requestBody->seek(0);
                }
                sendRequest(
                    verb,
                    redirectUrl,
                    reply()->request(),
                    _requestBody);
                return;
            }
        }
    }

    AbstractCredentials *creds = _account->credentials();
    if (!creds->stillValid(_reply) && !_ignoreCredentialFailure) {
        _account->handleInvalidCredentials();
    }

    bool discard = finished();
    if (discard) {
        qCDebug(lcNetworkJob) << "Network job" << metaObject()->className() << "finished for" << path();
        deleteLater();
    }
}

QByteArray AbstractNetworkJob::responseTimestamp()
{
    ASSERT(!_responseTimestamp.isEmpty());
    return _responseTimestamp;
}

QByteArray AbstractNetworkJob::requestId()
{
    return  _reply ? _reply->request().rawHeader("X-Request-ID") : QByteArray();
}

QString AbstractNetworkJob::errorString() const
{
    if (_timedout) {
        return tr("Connection timed out");
    } else if (!reply()) {
        return tr("Unknown error: network reply was deleted");
    } else if (reply()->hasRawHeader("OC-ErrorString")) {
        return reply()->rawHeader("OC-ErrorString");
    } else {
        return networkReplyErrorString(*reply());
    }
}

QString AbstractNetworkJob::errorStringParsingBody(QByteArray *body)
{
    QString base = errorString();
    if (base.isEmpty() || !reply()) {
        return QString();
    }

    QByteArray replyBody = reply()->readAll();
    if (body) {
        *body = replyBody;
    }

    QString extra = extractErrorMessage(replyBody);
    // Don't append the XML error message to a OC-ErrorString message.
    if (!extra.isEmpty() && !reply()->hasRawHeader("OC-ErrorString")) {
        return QString::fromLatin1("%1 (%2)").arg(base, extra);
    }

    return base;
}

AbstractNetworkJob::~AbstractNetworkJob()
{
    setReply(nullptr);
}

void AbstractNetworkJob::start()
{
    _timer.start();

    const QUrl url = account()->url();
    const QString displayUrl = QString("%1://%2%3").arg(url.scheme()).arg(url.host()).arg(url.path());

    QString parentMetaObjectName = parent() ? parent()->metaObject()->className() : "";
    qCInfo(lcNetworkJob) << metaObject()->className() << "created for" << displayUrl << "+" << path() << parentMetaObjectName;
}

void AbstractNetworkJob::slotTimeout()
{
    _timedout = true;
    qCWarning(lcNetworkJob) << "Network job timeout" << (reply() ? reply()->request().url() : path());
    onTimedOut();
}

void AbstractNetworkJob::onTimedOut()
{
    abort();
}

QString AbstractNetworkJob::replyStatusString() {
    Q_ASSERT(reply());
    if (reply()->error() == QNetworkReply::NoError) {
        return QLatin1String("OK");
    } else {
        QString enumStr = QMetaEnum::fromType<QNetworkReply::NetworkError>().valueToKey(static_cast<int>(reply()->error()));
        return QStringLiteral("%1 %2").arg(enumStr, errorString());
    }
}

NetworkJobTimeoutPauser::NetworkJobTimeoutPauser(QNetworkReply *reply)
{
    _timer = reply->property("timer").value<QTimer *>();
    if (!_timer.isNull()) {
        _timer->stop();
    }
}

NetworkJobTimeoutPauser::~NetworkJobTimeoutPauser()
{
    if (!_timer.isNull()) {
        _timer->start();
    }
}

QString extractErrorMessage(const QByteArray &errorResponse)
{
    QXmlStreamReader reader(errorResponse);
    reader.readNextStartElement();
    if (reader.name() != "error") {
        return QString();
    }

    QString exception;
    while (!reader.atEnd() && !reader.hasError()) {
        reader.readNextStartElement();
        if (reader.name() == QLatin1String("message")) {
            QString message = reader.readElementText();
            if (!message.isEmpty()) {
                return message;
            }
        } else if (reader.name() == QLatin1String("exception")) {
            exception = reader.readElementText();
        }
    }
    // Fallback, if message could not be found
    return exception;
}

QString errorMessage(const QString &baseError, const QByteArray &body)
{
    QString msg = baseError;
    QString extra = extractErrorMessage(body);
    if (!extra.isEmpty()) {
        msg += QString::fromLatin1(" (%1)").arg(extra);
    }
    return msg;
}

QString networkReplyErrorString(const QNetworkReply &reply)
{
    QString base = reply.errorString();
    int httpStatus = reply.attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString httpReason = reply.attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();

    // Only adjust HTTP error messages of the expected format.
    if (httpReason.isEmpty() || httpStatus == 0 || !base.contains(httpReason)) {
        return base;
    }

    return AbstractNetworkJob::tr(R"(Server replied "%1 %2" to "%3 %4")").arg(QString::number(httpStatus), httpReason, HttpLogger::requestVerb(reply), reply.request().url().toDisplayString());
}

void AbstractNetworkJob::retry()
{
    ENFORCE(_reply);
    auto req = _reply->request();
    QUrl requestedUrl = req.url();
    QByteArray verb = HttpLogger::requestVerb(*_reply);
    qCInfo(lcNetworkJob) << "Restarting" << verb << requestedUrl;
    resetTimeout();
    if (_requestBody) {
        _requestBody->seek(0);
    }
    sendRequest(_verb, _request.url(), _request, _requestBody);
}

void AbstractNetworkJob::abort()
{
    if (_reply) {
        _reply->abort();
    } else {
        deleteLater();
    }
}

} // namespace OCC
