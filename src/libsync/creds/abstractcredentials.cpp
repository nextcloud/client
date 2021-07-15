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

#include <QLoggingCategory>
#include <QString>
#include <QCoreApplication>

#include "common/asserts.h"
#include "creds/abstractcredentials.h"

namespace OCC {

Q_LOGGING_CATEGORY(lcCredentials, "nextcloud.sync.credentials", QtInfoMsg)

AbstractCredentials::AbstractCredentials() = default;

void AbstractCredentials::setAccount(Account *account)
{
    ENFORCE(!_account, "should only setAccount once");
    _account = account;
}

QString AbstractCredentials::keychainKey(const QString &url, const QString &user, const QString &accountId)
{
    QString u(url);
    if (u.isEmpty()) {
        qCWarning(lcCredentials) << "Empty url in keyChain, error!";
        return QString();
    }
    if (user.isEmpty()) {
        qCWarning(lcCredentials) << "Error: User is empty!";
        return QString();
    }

    if (!u.endsWith(QChar('/'))) {
        u.append(QChar('/'));
    }

    QString key = user + QLatin1Char(':') + u;
    if (!accountId.isEmpty()) {
        key += QLatin1Char(':') + accountId;
    }
#ifdef Q_OS_WIN
        // On Windows the credential keys aren't namespaced properly
        // by qtkeychain. To work around that we manually add namespacing
        // to the generated keys. See #6125.
        // It's safe to do that since the key format is changing for 2.4
        // anyway to include the account ids. That means old keys can be
        // migrated to new namespaced keys on windows for 2.4.
        key.prepend(QCoreApplication::applicationName() + "_");
#endif
    return key;
}
} // namespace OCC
