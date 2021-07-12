/*
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
 * Copyright (C) by Michael Schuster <michael@schuster.ms>
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

#include <QDesktopServices>
#include <QApplication>
#include <QClipboard>
#include <QTimer>
#include <QBuffer>
#include "account.h"
#include "flow2auth.h"
#include <QJsonObject>
#include <QJsonDocument>
#include "theme.h"
#include "networkjobs.h"
#include "configfile.h"
#include "guiutility.h"

namespace OCC {

Q_LOGGING_CATEGORY(lcFlow2auth, "nextcloud.sync.credentials.flow2auth", QtInfoMsg)


Flow2Auth::Flow2Auth(Account *account, QObject *parent)
    : QObject(parent)
    , _account(account)
    , _isBusy(false)
    , _hasToken(false)
{
    _pollTimer.setInterval(1000);
    QObject::connect(&_pollTimer, &QTimer::timeout, this, &Flow2Auth::slotPollTimerTimeout);
}

Flow2Auth::~Flow2Auth() = default;

void Flow2Auth::start()
{
    // Note: All startup code is in openBrowser() to allow reinitiate a new request with
    //       fresh tokens. Opening the same pollEndpoint link twice triggers an expiration
    //       message by the server (security, intended design).
    openBrowser();
}

QUrl Flow2Auth::authorisationLink() const
{
    return _loginUrl;
}

void Flow2Auth::openBrowser()
{
    fetchNewToken(TokenAction::actionOpenBrowser);
}

void Flow2Auth::copyLinkToClipboard()
{
    fetchNewToken(TokenAction::actionCopyLinkToClipboard);
}

void Flow2Auth::fetchNewToken(const TokenAction action)
{
    if(_isBusy)
        return;

    _isBusy = true;
    _hasToken = false;

    emit statusChanged(PollStatus::statusFetchToken, 0);

    // Step 1: Initiate a login, do an anonymous POST request
    QUrl url = Utility::concatUrlPath(_account->url().toString(), QLatin1String("/index.php/login/v2"));

    // add 'Content-Length: 0' header (see https://github.com/nextcloud/desktop/issues/1473)
    QNetworkRequest req;
    req.setHeader(QNetworkRequest::ContentLengthHeader, "0");
    req.setHeader(QNetworkRequest::UserAgentHeader, Utility::friendlyUserAgentString());

    auto job = _account->sendRequest("POST", url, req);
    job->setTimeout(qMin(30 * 1000ll, job->timeoutMsec()));

    QObject::connect(job, &SimpleNetworkJob::finishedSignal, this, [this, action](QNetworkReply *reply) {
        auto jsonData = reply->readAll();
        QJsonParseError jsonParseError;
        QJsonObject json = QJsonDocument::fromJson(jsonData, &jsonParseError).object();
        QString pollToken, pollEndpoint, loginUrl;

        if (reply->error() == QNetworkReply::NoError && jsonParseError.error == QJsonParseError::NoError
            && !json.isEmpty()) {
            pollToken = json.value("poll").toObject().value("token").toString();
            pollEndpoint = json.value("poll").toObject().value("endpoint").toString();
            loginUrl = json["login"].toString();
        }

        if (reply->error() != QNetworkReply::NoError || jsonParseError.error != QJsonParseError::NoError
            || json.isEmpty() || pollToken.isEmpty() || pollEndpoint.isEmpty() || loginUrl.isEmpty()) {
            QString errorReason;
            QString errorFromJson = json["error"].toString();
            if (!errorFromJson.isEmpty()) {
                errorReason = tr("Error returned from the server: <em>%1</em>")
                                  .arg(errorFromJson.toHtmlEscaped());
            } else if (reply->error() != QNetworkReply::NoError) {
                errorReason = tr("There was an error accessing the \"token\" endpoint: <br><em>%1</em>")
                                  .arg(reply->errorString().toHtmlEscaped());
            } else if (jsonParseError.error != QJsonParseError::NoError) {
                errorReason = tr("Could not parse the JSON returned from the server: <br><em>%1</em>")
                                  .arg(jsonParseError.errorString());
            } else {
                errorReason = tr("The reply from the server did not contain all expected fields");
            }
            qCWarning(lcFlow2auth) << "Error when getting the loginUrl" << json << errorReason;
            emit result(Error, errorReason);
            _pollTimer.stop();
            _isBusy = false;
            return;
        }


        _loginUrl = loginUrl;
        _pollToken = pollToken;
        _pollEndpoint = pollEndpoint;


        // Start polling
        ConfigFile cfg;
        std::chrono::milliseconds polltime = cfg.remotePollInterval();
        qCInfo(lcFlow2auth) << "setting remote poll timer interval to" << polltime.count() << "msec";
        _secondsInterval = (polltime.count() / 1000);
        _secondsLeft = _secondsInterval;
        emit statusChanged(PollStatus::statusPollCountdown, _secondsLeft);

        if(!_pollTimer.isActive()) {
            _pollTimer.start();
        }


        switch(action)
        {
        case actionOpenBrowser:
            // Try to open Browser
            if (!Utility::openBrowser(authorisationLink())) {
                // We cannot open the browser, then we claim we don't support Flow2Auth.
                // Our UI callee will ask the user to copy and open the link.
                emit result(NotSupported);
            }
            break;
        case actionCopyLinkToClipboard:
            QApplication::clipboard()->setText(authorisationLink().toString(QUrl::FullyEncoded));
            emit statusChanged(PollStatus::statusCopyLinkToClipboard, 0);
            break;
        }

        _isBusy = false;
        _hasToken = true;
    });
}

void Flow2Auth::slotPollTimerTimeout()
{
    if(_isBusy || !_hasToken)
        return;

    _isBusy = true;

    _secondsLeft--;
    if(_secondsLeft > 0) {
        emit statusChanged(PollStatus::statusPollCountdown, _secondsLeft);
        _isBusy = false;
        return;
    }
    emit statusChanged(PollStatus::statusPollNow, 0);

    // Step 2: Poll
    QNetworkRequest req;
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

    auto requestBody = new QBuffer;
    QUrlQuery arguments(QString("token=%1").arg(_pollToken));
    requestBody->setData(arguments.query(QUrl::FullyEncoded).toLatin1());

    auto job = _account->sendRequest("POST", _pollEndpoint, req, requestBody);
    job->setTimeout(qMin(30 * 1000ll, job->timeoutMsec()));

    QObject::connect(job, &SimpleNetworkJob::finishedSignal, this, [this](QNetworkReply *reply) {
        auto jsonData = reply->readAll();
        QJsonParseError jsonParseError;
        QJsonObject json = QJsonDocument::fromJson(jsonData, &jsonParseError).object();
        QUrl serverUrl;
        QString loginName, appPassword;

        if (reply->error() == QNetworkReply::NoError && jsonParseError.error == QJsonParseError::NoError
            && !json.isEmpty()) {
            serverUrl = json["server"].toString();
            loginName = json["loginName"].toString();
            appPassword = json["appPassword"].toString();
        }

        if (reply->error() != QNetworkReply::NoError || jsonParseError.error != QJsonParseError::NoError
            || json.isEmpty() || serverUrl.isEmpty() || loginName.isEmpty() || appPassword.isEmpty()) {
            QString errorReason;
            QString errorFromJson = json["error"].toString();
            if (!errorFromJson.isEmpty()) {
                errorReason = tr("Error returned from the server: <em>%1</em>")
                                  .arg(errorFromJson.toHtmlEscaped());
            } else if (reply->error() != QNetworkReply::NoError) {
                errorReason = tr("There was an error accessing the \"token\" endpoint: <br><em>%1</em>")
                                  .arg(reply->errorString().toHtmlEscaped());
            } else if (jsonParseError.error != QJsonParseError::NoError) {
                errorReason = tr("Could not parse the JSON returned from the server: <br><em>%1</em>")
                                  .arg(jsonParseError.errorString());
            } else {
                errorReason = tr("The reply from the server did not contain all expected fields");
            }
            qCDebug(lcFlow2auth) << "Error when polling for the appPassword" << json << errorReason;

            // We get a 404 until authentication is done, so don't show this error in the GUI.
            if(reply->error() != QNetworkReply::ContentNotFoundError)
                emit result(Error, errorReason);

            // Forget sensitive data
            appPassword.clear();
            loginName.clear();

            // Failed: poll again
            _secondsLeft = _secondsInterval;
            _isBusy = false;
            return;
        }

        _pollTimer.stop();

        // Success
        qCInfo(lcFlow2auth) << "Success getting the appPassword for user: " << loginName << ", server: " << serverUrl.toString();

        _account->setUrl(serverUrl);

        emit result(LoggedIn, QString(), loginName, appPassword);

        // Forget sensitive data
        appPassword.clear();
        loginName.clear();

        _loginUrl.clear();
        _pollToken.clear();
        _pollEndpoint.clear();

        _isBusy = false;
        _hasToken = false;
    });
}

void Flow2Auth::slotPollNow()
{
    // poll now if we're not already doing so
    if(_isBusy || !_hasToken)
        return;

    _secondsLeft = 1;
    slotPollTimerTimeout();
}

} // namespace OCC
