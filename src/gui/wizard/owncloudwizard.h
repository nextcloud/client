/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#ifndef MIRALL_OWNCLOUD_WIZARD_H
#define MIRALL_OWNCLOUD_WIZARD_H

#include <QWizard>
#include <QLoggingCategory>
#include <QSslKey>
#include <QSslCertificate>

#include "networkjobs.h"
#include "wizard/owncloudwizardcommon.h"
#include "accountfwd.h"

namespace OCC {

Q_DECLARE_LOGGING_CATEGORY(lcWizard)

class OwncloudSetupPage;
class OwncloudHttpCredsPage;
class OwncloudOAuthCredsPage;
#ifndef NO_SHIBBOLETH
class OwncloudShibbolethCredsPage;
#endif
class OwncloudAdvancedSetupPage;
class OwncloudWizardResultPage;
class AbstractCredentials;
class AbstractCredentialsWizardPage;
class WebViewPage;
class Flow2AuthCredsPage;

/**
 * @brief The OwncloudWizard class
 * @ingroup gui
 */
class OwncloudWizard : public QWizard
{
    Q_OBJECT
public:
    enum LogType {
        LogPlain,
        LogParagraph
    };

    OwncloudWizard(QWidget *parent = nullptr);

    void setAccount(AccountPtr account);
    AccountPtr account() const;
    void setOCUrl(const QString &);
    bool registration();
    void setRegistration(bool registration);

    void setupCustomMedia(QVariant, QLabel *);
    QString ocUrl() const;
    QString localFolder() const;
    QStringList selectiveSyncBlacklist() const;
    bool isConfirmBigFolderChecked() const;

    void enableFinishOnResultWidget(bool enable);

    void displayError(const QString &, bool retryHTTPonly);
    AbstractCredentials *getCredentials() const;

    void bringToTop();

    // FIXME: Can those be local variables?
    // Set from the OwncloudSetupPage, later used from OwncloudHttpCredsPage
    QSslKey _clientSslKey;
    QSslCertificate _clientSslCertificate;
    QList<QSslCertificate> _clientSslCaCertificates;

public slots:
    void setAuthType(DetermineAuthTypeJob::AuthType type);
    void setRemoteFolder(const QString &);
    void appendToConfigurationLog(const QString &msg, LogType type = LogParagraph);
    void slotCurrentPageChanged(int);
    void successfulStep();

signals:
    void clearPendingRequests();
    void determineAuthType(const QString &);
    void connectToOCUrl(const QString &);
    void createLocalAndRemoteFolders(const QString &, const QString &);
    // make sure to connect to this, rather than finished(int)!!
    void basicSetupFinished(int);
    void skipFolderConfiguration();
    void needCertificate();
    void styleChanged();
    void onActivate();
    void onSetVisible(bool visible);

protected:
    void changeEvent(QEvent *) override;
    void setVisible(bool visible) override;

private:
    void customizeStyle();

    AccountPtr _account;
    OwncloudSetupPage *_setupPage;
    OwncloudHttpCredsPage *_httpCredsPage;
    OwncloudOAuthCredsPage *_browserCredsPage;
#ifndef NO_SHIBBOLETH
    OwncloudShibbolethCredsPage *_shibbolethCredsPage;
#endif
    Flow2AuthCredsPage *_flow2CredsPage;
    OwncloudAdvancedSetupPage *_advancedSetupPage;
    OwncloudWizardResultPage *_resultPage;
    AbstractCredentialsWizardPage *_credentialsPage = nullptr;
    WebViewPage *_webViewPage;

    QStringList _setupLog;

    bool _registration = false;

    friend class OwncloudSetupWizard;
};

} // namespace OCC

#endif
