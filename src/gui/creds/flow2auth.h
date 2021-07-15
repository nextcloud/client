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

#pragma once
#include <QPointer>
#include <QUrl>
#include <QTimer>
#include "accountfwd.h"

namespace OCC {

/**
 * Job that does the authorization, grants and fetches the access token via Login Flow v2
 *
 * See: https://docs.nextcloud.com/server/latest/developer_manual/client_apis/LoginFlow/index.html#login-flow-v2
 */
class Flow2Auth : public QObject
{
    Q_OBJECT
public:
    enum TokenAction {
        actionOpenBrowser = 1,
        actionCopyLinkToClipboard
    };
    enum PollStatus {
        statusPollCountdown = 1,
        statusPollNow,
        statusFetchToken,
        statusCopyLinkToClipboard
    };

    Flow2Auth(Account *account, QObject *parent);
    ~Flow2Auth();

    enum Result { NotSupported,
        LoggedIn,
        Error };
    Q_ENUM(Result);
    void start();
    void openBrowser();
    void copyLinkToClipboard();
    QUrl authorisationLink() const;

signals:
    /**
     * The state has changed.
     * when logged in, appPassword has the value of the app password.
     */
    void result(Flow2Auth::Result result, const QString &errorString = QString(),
                const QString &user = QString(), const QString &appPassword = QString());

    void statusChanged(const PollStatus status, int secondsLeft);

public slots:
    void slotPollNow();

private slots:
    void slotPollTimerTimeout();

private:
    void fetchNewToken(const TokenAction action);

    Account *_account;
    QUrl _loginUrl;
    QString _pollToken;
    QString _pollEndpoint;
    QTimer _pollTimer;
    qint64 _secondsLeft;
    qint64 _secondsInterval;
    bool _isBusy;
    bool _hasToken;
    bool _enforceHttps = false;
};

} // namespace OCC
