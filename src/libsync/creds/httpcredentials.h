/*
 * Copyright (C) by Klaas Freitag <freitag@kde.org>
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

#ifndef MIRALL_CREDS_HTTP_CREDENTIALS_H
#define MIRALL_CREDS_HTTP_CREDENTIALS_H

#include <QMap>
#include <QSslCertificate>
#include <QSslKey>
#include <QNetworkRequest>
#include "creds/abstractcredentials.h"

class QNetworkReply;
class QAuthenticator;

namespace QKeychain {
class Job;
class WritePasswordJob;
class ReadPasswordJob;
}

namespace OCC {

/*
   The authentication system is this way because of Shibboleth.
   There used to be two different ways to authenticate: Shibboleth and HTTP Basic Auth.
   AbstractCredentials can be inherited from both ShibbolethCrendentials and HttpCredentials.

   HttpCredentials is then split in HttpCredentials and HttpCredentialsGui.

   This class handle both HTTP Basic Auth and OAuth. But anything that needs GUI to ask the user
   is in HttpCredentialsGui.

   The authentication mechanism looks like this.

   1) First, AccountState will attempt to load the certificate from the keychain

   ---->  fetchFromKeychain
                |                           }
                v                            }
          slotReadClientCertPEMJobDone       }     There are first 3 QtKeychain jobs to fetch
                |                             }   the TLS client keys, if any, and the password
                v                            }      (or refresh token
          slotReadClientKeyPEMJobDone        }
                |                           }
                v
            slotReadJobDone
                |        |
                |        +-------> emit fetched()   if OAuth is not used
                |
                v
            refreshAccessToken()
                |
                v
            emit fetched()

   2) If the credentials is still not valid when fetched() is emitted, the ui, will call askFromUser()
      which is implemented in HttpCredentialsGui

 */
class OWNCLOUDSYNC_EXPORT HttpCredentials : public AbstractCredentials
{
    Q_OBJECT
    friend class HttpCredentialsAccessManager;

public:
    /// Don't add credentials if this is set on a QNetworkRequest
    static constexpr QNetworkRequest::Attribute DontAddCredentialsAttribute = QNetworkRequest::User;

    HttpCredentials();
    explicit HttpCredentials(const QString &user, const QString &password,
            const QByteArray &clientCertBundle = QByteArray(), const QByteArray &clientCertPassword = QByteArray());

    QString authType() const override;
    QNetworkAccessManager *createQNAM() const override;
    bool ready() const override;
    void fetchFromKeychain() override;
    bool stillValid(QNetworkReply *reply) override;
    void persist() override;
    QString user() const override;
    // the password or token
    QString password() const override;
    void invalidateToken() override;
    void forgetSensitiveData() override;
    QString fetchUser();
    virtual bool sslIsTrusted() { return false; }

    /* If we still have a valid refresh token, try to refresh it assynchronously and emit fetched()
     * otherwise return false
     */
    bool refreshAccessToken();

    // To fetch the user name as early as possible
    void setAccount(Account *account) override;

    // Whether we are using OAuth
    bool isUsingOAuth() const { return !_refreshToken.isNull(); }

    bool retryIfNeeded(AbstractNetworkJob *) override;

private Q_SLOTS:
    void slotAuthentication(QNetworkReply *, QAuthenticator *);

    void slotReadClientCertPasswordJobDone(QKeychain::Job *);
    void slotReadClientCertPEMJobDone(QKeychain::Job *);
    void slotReadClientKeyPEMJobDone(QKeychain::Job *);

    void slotReadPasswordFromKeychain();
    void slotReadJobDone(QKeychain::Job *);

    void slotWriteClientCertPasswordJobDone(QKeychain::Job *);
    void slotWriteClientCertPEMJobDone(QKeychain::Job *);
    void slotWriteClientKeyPEMJobDone(QKeychain::Job *);

    void slotWritePasswordToKeychain();
    void slotWriteJobDone(QKeychain::Job *);

protected:
    /** Reads data from keychain locations
     *
     * Goes through
     *   slotReadClientCertPEMJobDone to
     *   slotReadClientCertPEMJobDone to
     *   slotReadJobDone
     */
    void fetchFromKeychainHelper();

    /// Wipes legacy keychain locations
    void deleteOldKeychainEntries();

    /** Whether to bow out now because a retry will happen later
     *
     * Sometimes the keychain needs a while to become available.
     * This function should be called on first keychain-read to check
     * whether it errored because the keychain wasn't available yet.
     * If that happens, this function will schedule another try and
     * return true.
     */
    bool keychainUnavailableRetryLater(QKeychain::ReadPasswordJob *);

    /** Takes client cert pkcs12 and unwraps the key/cert.
     *
     * Returns false on failure.
     */
    bool unpackClientCertBundle();

    QString _user;
    QString _password; // user's password, or access_token for OAuth
    QString _refreshToken; // OAuth _refreshToken, set if OAuth is used.
    QString _previousPassword;

    QString _fetchErrorString;
    bool _ready = false;
    bool _isRenewingOAuthToken = false;
    QByteArray _clientCertBundle;
    QByteArray _clientCertPassword;
    QSslKey _clientSslKey;
    QSslCertificate _clientSslCertificate;
    bool _keychainMigration = false;
    bool _retryOnKeyChainError = true; // true if we haven't done yet any reading from keychain

    QVector<QPointer<AbstractNetworkJob>> _retryQueue; // Jobs we need to retry once the auth token is fetched
};


} // namespace OCC

#endif
