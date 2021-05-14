/*
 * Copyright (C) by Krzesimir Nowak <krzesimir@endocode.com>
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

#ifndef MIRALL_CREDS_ABSTRACT_CREDENTIALS_H
#define MIRALL_CREDS_ABSTRACT_CREDENTIALS_H

#include <QObject>

#include <csync.h>
#include "owncloudlib.h"
#include "accountfwd.h"

class QNetworkAccessManager;
class QNetworkReply;
namespace OCC {

class AbstractNetworkJob;

class OWNCLOUDSYNC_EXPORT AbstractCredentials : public QObject
{
    Q_OBJECT

public:
    AbstractCredentials();
    // No need for virtual destructor - QObject already has one.

    /** The bound account for the credentials instance.
     *
     * Credentials are always used in conjunction with an account.
     * Calling Account::setCredentials() will call this function.
     * Credentials only live as long as the underlying account object.
     */
    virtual void setAccount(Account *account);

    virtual QString authType() const = 0;
    virtual QString user() const = 0;
    virtual QString password() const = 0;
    virtual QNetworkAccessManager *createQNAM() const = 0;

    /** Whether there are credentials that can be used for a connection attempt. */
    virtual bool ready() const = 0;

    /** Whether fetchFromKeychain() was called before. */
    bool wasFetched() const { return _wasFetched; }

    /** Trigger (async) fetching of credential information
     *
     * Should set _wasFetched = true, and later emit fetched() when done.
     */
    virtual void fetchFromKeychain() = 0;

    /** Ask credentials from the user (typically async)
     *
     * Should emit asked() when done.
     */
    virtual void askFromUser() = 0;

    virtual bool stillValid(QNetworkReply *reply) = 0;
    virtual void persist() = 0;

    /** Invalidates token used to authorize requests, it will no longer be used.
     *
     * For http auth, this would be the session cookie.
     *
     * Note that sensitive data (like the password used to acquire the
     * session cookie) may be retained. See forgetSensitiveData().
     *
     * ready() must return false afterwards.
     */
    virtual void invalidateToken() = 0;

    /** Clears out all sensitive data; used for fully signing out users.
     *
     * This should always imply invalidateToken() but may go beyond it.
     *
     * For http auth, this would clear the session cookie and password.
     */
    virtual void forgetSensitiveData() = 0;

    static QString keychainKey(const QString &url, const QString &user, const QString &accountId);

Q_SIGNALS:
    /** Emitted when fetchFromKeychain() is done.
     *
     * Note that ready() can be true or false, depending on whether there was useful
     * data in the keychain.
     */
    // TODO: rename
    void fetched();

    /** Emitted when askFromUser() is done.
     *
     * Note that ready() can be true or false, depending on whether the user provided
     * data or not.
     */
    void asked();

    void authenticationStarted();
    void authenticationFailed();

protected:
    Account *_account = nullptr;
    bool _wasFetched = false;
};

} // namespace OCC

#endif
