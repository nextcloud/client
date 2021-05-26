/*
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
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

#include "accountmanager.h"
#include "configfile.h"
#include "sslerrordialog.h"
#include "proxyauthhandler.h"
#include "common/asserts.h"
#include <theme.h>
#include <creds/credentialsfactory.h>
#include <creds/abstractcredentials.h>
#include <cookiejar.h>
#include <QSettings>
#include <QDir>
#include <QNetworkAccessManager>
#include <QMessageBox>
#include "clientsideencryption.h"
#include "ui_mnemonicdialog.h"

namespace {
static const char urlC[] = "url";
static const char authTypeC[] = "authType";
static const char userC[] = "user";
static const char httpUserC[] = "http_user";
static const char davUserC[] = "dav_user";
static const char caCertsKeyC[] = "CaCertificates";
static const char accountsC[] = "Accounts";
static const char versionC[] = "version";
static const char serverVersionC[] = "serverVersion";

// The maximum versions that this client can read
static const int maxAccountsVersion = 2;
static const int maxAccountVersion = 1;
}


namespace OCC {

Q_LOGGING_CATEGORY(lcAccountManager, "nextcloud.gui.account.manager", QtInfoMsg)

AccountManager *AccountManager::instance()
{
    static AccountManager instance;
    return &instance;
}

bool AccountManager::restore()
{
    QStringList skipSettingsKeys;
    backwardMigrationSettingsKeys(&skipSettingsKeys, &skipSettingsKeys);

    auto settings = ConfigFile::settingsWithGroup(QLatin1String(accountsC));
    if (settings->status() != QSettings::NoError || !settings->isWritable()) {
        qCWarning(lcAccountManager) << "Could not read settings from" << settings->fileName()
                                    << settings->status();
        return false;
    }

    if (skipSettingsKeys.contains(settings->group())) {
        // Should not happen: bad container keys should have been deleted
        qCWarning(lcAccountManager) << "Accounts structure is too new, ignoring";
        return true;
    }

    // If there are no accounts, check the old format.
    if (settings->childGroups().isEmpty()
        && !settings->contains(QLatin1String(versionC))) {
        restoreFromLegacySettings();
        return true;
    }

    for (const auto &accountId : settings->childGroups()) {
        settings->beginGroup(accountId);
        if (!skipSettingsKeys.contains(settings->group())) {
            if (auto acc = loadAccountHelper(*settings)) {
                acc->_id = accountId;
                if (auto accState = AccountState::loadFromSettings(acc, *settings)) {
                    auto jar = qobject_cast<CookieJar*>(acc->_am->cookieJar());
                    ASSERT(jar);
                    if (jar)
                        jar->restore(acc->cookieJarPath());
                    addAccountState(accState);
                }
            }
        } else {
            qCInfo(lcAccountManager) << "Account" << accountId << "is too new, ignoring";
            _additionalBlockedAccountIds.insert(accountId);
        }
        settings->endGroup();
    }

    return true;
}

void AccountManager::backwardMigrationSettingsKeys(QStringList *deleteKeys, QStringList *ignoreKeys)
{
    auto settings = ConfigFile::settingsWithGroup(QLatin1String(accountsC));
    const int accountsVersion = settings->value(QLatin1String(versionC)).toInt();
    if (accountsVersion <= maxAccountsVersion) {
        foreach (const auto &accountId, settings->childGroups()) {
            settings->beginGroup(accountId);
            const int accountVersion = settings->value(QLatin1String(versionC), 1).toInt();
            if (accountVersion > maxAccountVersion) {
                ignoreKeys->append(settings->group());
            }
            settings->endGroup();
        }
    } else {
        deleteKeys->append(settings->group());
    }
}

bool AccountManager::restoreFromLegacySettings()
{
    qCInfo(lcAccountManager) << "Migrate: restoreFromLegacySettings, checking settings group"
                             << Theme::instance()->appName();

    // try to open the correctly themed settings
    auto settings = ConfigFile::settingsWithGroup(Theme::instance()->appName());

    if (settings->childKeys().isEmpty()) {
        qCInfo(lcAccountManager) << "Migrate: restoreFromLegacySettings, checking settings group"
                                << Theme::instance()->appRevDomain();

        // try to open the correctly themed settings
        settings = ConfigFile::settingsWithGroup(Theme::instance()->appRevDomain());
    }

    // if the settings file could not be opened, the childKeys list is empty
    // then try to load settings from a very old place
    if (settings->childKeys().isEmpty()) {
        // Now try to open the original ownCloud settings to see if they exist.
        QString oCCfgFile = QDir::fromNativeSeparators(settings->fileName());
        // replace the last two segments with ownCloud/owncloud.cfg
        oCCfgFile = oCCfgFile.left(oCCfgFile.lastIndexOf('/'));
        oCCfgFile = oCCfgFile.left(oCCfgFile.lastIndexOf('/'));
        oCCfgFile += QLatin1String("/ownCloud/owncloud.cfg");

        qCInfo(lcAccountManager) << "Migrate: checking old config " << oCCfgFile;

        QFileInfo fi(oCCfgFile);
        if (fi.isReadable()) {
            std::unique_ptr<QSettings> oCSettings(new QSettings(oCCfgFile, QSettings::IniFormat));
            oCSettings->beginGroup(QLatin1String("ownCloud"));

            // Check the theme url to see if it is the same url that the oC config was for
            QString overrideUrl = Theme::instance()->overrideServerUrl();
            if (!overrideUrl.isEmpty()) {
                if (overrideUrl.endsWith('/')) {
                    overrideUrl.chop(1);
                }
                QString oCUrl = oCSettings->value(QLatin1String(urlC)).toString();
                if (oCUrl.endsWith('/')) {
                    oCUrl.chop(1);
                }

                // in case the urls are equal reset the settings object to read from
                // the ownCloud settings object
                qCInfo(lcAccountManager) << "Migrate oC config if " << oCUrl << " == " << overrideUrl << ":"
                                         << (oCUrl == overrideUrl ? "Yes" : "No");
                if (oCUrl == overrideUrl) {
                    settings = std::move(oCSettings);
                }
            }
        }
    }

    // Try to load the single account.
    if (!settings->childKeys().isEmpty()) {
        if (auto acc = loadAccountHelper(*settings)) {
            addAccount(acc);
            return true;
        }
    }
    return false;
}

void AccountManager::save(bool saveCredentials)
{
    auto settings = ConfigFile::settingsWithGroup(QLatin1String(accountsC));
    settings->setValue(QLatin1String(versionC), maxAccountsVersion);
    for (const auto &acc : qAsConst(_accounts)) {
        settings->beginGroup(acc->account()->id());
        saveAccountHelper(acc->account().data(), *settings, saveCredentials);
        acc->writeToSettings(*settings);
        settings->endGroup();
    }

    settings->sync();
    qCInfo(lcAccountManager) << "Saved all account settings, status:" << settings->status();
}

void AccountManager::saveAccount(Account *a)
{
    qCDebug(lcAccountManager) << "Saving account" << a->url().toString();
    auto settings = ConfigFile::settingsWithGroup(QLatin1String(accountsC));
    settings->beginGroup(a->id());
    saveAccountHelper(a, *settings, false); // don't save credentials they might not have been loaded yet
    settings->endGroup();

    settings->sync();
    qCDebug(lcAccountManager) << "Saved account settings, status:" << settings->status();
}

void AccountManager::saveAccountState(AccountState *a)
{
    qCDebug(lcAccountManager) << "Saving account state" << a->account()->url().toString();
    auto settings = ConfigFile::settingsWithGroup(QLatin1String(accountsC));
    settings->beginGroup(a->account()->id());
    a->writeToSettings(*settings);
    settings->endGroup();

    settings->sync();
    qCDebug(lcAccountManager) << "Saved account state settings, status:" << settings->status();
}

void AccountManager::saveAccountHelper(Account *acc, QSettings &settings, bool saveCredentials)
{
    settings.setValue(QLatin1String(versionC), maxAccountVersion);
    settings.setValue(QLatin1String(urlC), acc->_url.toString());
    settings.setValue(QLatin1String(davUserC), acc->_davUser);
    settings.setValue(QLatin1String(serverVersionC), acc->_serverVersion);
    if (acc->_credentials) {
        if (saveCredentials) {
            // Only persist the credentials if the parameter is set, on migration from 1.8.x
            // we want to save the accounts but not overwrite the credentials
            // (This is easier than asynchronously fetching the credentials from keychain and then
            // re-persisting them)
            acc->_credentials->persist();
        }
        for (const auto &key : acc->_settingsMap.keys()) {
            settings.setValue(key, acc->_settingsMap.value(key));
        }
        settings.setValue(QLatin1String(authTypeC), acc->_credentials->authType());

        // HACK: Save http_user also as user
        if (acc->_settingsMap.contains(httpUserC))
            settings.setValue(userC, acc->_settingsMap.value(httpUserC));
    }

    // Save accepted certificates.
    settings.beginGroup(QLatin1String("General"));
    qCInfo(lcAccountManager) << "Saving " << acc->approvedCerts().count() << " unknown certs.";
    QByteArray certs;
    for (const auto &cert : acc->approvedCerts()) {
        certs += cert.toPem() + '\n';
    }
    if (!certs.isEmpty()) {
        settings.setValue(QLatin1String(caCertsKeyC), certs);
    }
    settings.endGroup();

    // Save cookies.
    if (acc->_am) {
        auto *jar = qobject_cast<CookieJar *>(acc->_am->cookieJar());
        if (jar) {
            qCInfo(lcAccountManager) << "Saving cookies." << acc->cookieJarPath();
            if (!jar->save(acc->cookieJarPath()))
            {
                qCWarning(lcAccountManager) << "Failed to save cookies to" << acc->cookieJarPath();
            }
        }
    }
}

AccountPtr AccountManager::loadAccountHelper(QSettings &settings)
{
    auto urlConfig = settings.value(QLatin1String(urlC));
    if (!urlConfig.isValid()) {
        // No URL probably means a corrupted entry in the account settings
        qCWarning(lcAccountManager) << "No URL for account " << settings.group();
        return AccountPtr();
    }

    auto acc = createAccount();

    QString authType = settings.value(QLatin1String(authTypeC)).toString();

    // There was an account-type saving bug when 'skip folder config' was used
    // See #5408. This attempts to fix up the "dummy" authType
    if (authType == QLatin1String("dummy")) {
        if (settings.contains(QLatin1String("http_user"))) {
            authType = "http";
        } else if (settings.contains(QLatin1String("shibboleth_shib_user"))) {
            authType = "shibboleth";
        }
    }

    QString overrideUrl = Theme::instance()->overrideServerUrl();
    QString forceAuth = Theme::instance()->forceConfigAuthType();
    if (!forceAuth.isEmpty() && !overrideUrl.isEmpty()) {
        // If forceAuth is set, this might also mean the overrideURL has changed.
        // See enterprise issues #1126
        acc->setUrl(overrideUrl);
        authType = forceAuth;
    } else {
        acc->setUrl(urlConfig.toUrl());
    }

    // Migrate to webflow
    if (authType == QLatin1String("http")) {
        authType = "webflow";
        settings.setValue(QLatin1String(authTypeC), authType);

        for (const QString &key : settings.childKeys()) {
            if (!key.startsWith("http_"))
                continue;
            auto newkey = QString::fromLatin1("webflow_").append(key.mid(5));
            settings.setValue(newkey, settings.value((key)));
            settings.remove(key);
        }
    }

    qCInfo(lcAccountManager) << "Account for" << acc->url() << "using auth type" << authType;

    acc->_serverVersion = settings.value(QLatin1String(serverVersionC)).toString();
    acc->_davUser = settings.value(QLatin1String(davUserC)).toString();

    // We want to only restore settings for that auth type and the user value
    acc->_settingsMap.insert(QLatin1String(userC), settings.value(userC));
    QString authTypePrefix = authType + "_";
    for (const auto &key : settings.childKeys()) {
        if (!key.startsWith(authTypePrefix))
            continue;
        acc->_settingsMap.insert(key, settings.value(key));
    }

    acc->setCredentials(CredentialsFactory::create(authType));

    // now the server cert, it is in the general group
    settings.beginGroup(QLatin1String("General"));
    const auto certs = QSslCertificate::fromData(settings.value(caCertsKeyC).toByteArray());
    qCInfo(lcAccountManager) << "Restored: " << certs.count() << " unknown certs.";
    acc->setApprovedCerts(certs);
    settings.endGroup();

    return acc;
}

AccountStatePtr AccountManager::account(const QString &name)
{
    const auto it = std::find_if(_accounts.cbegin(), _accounts.cend(), [name](const auto &acc) {
        return acc->account()->displayName() == name;
    });
    return it != _accounts.cend() ? *it : AccountStatePtr();
}

AccountState *AccountManager::addAccount(const AccountPtr &newAccount)
{
    auto id = newAccount->id();
    if (id.isEmpty() || !isAccountIdAvailable(id)) {
        id = generateFreeAccountId();
    }
    newAccount->_id = id;

    auto newAccountState = new AccountState(newAccount);
    addAccountState(newAccountState);
    return newAccountState;
}

void AccountManager::deleteAccount(AccountState *account)
{
    auto it = std::find(_accounts.begin(), _accounts.end(), account);
    if (it == _accounts.end()) {
        return;
    }
    auto copy = *it; // keep a reference to the shared pointer so it does not delete it just yet
    _accounts.erase(it);

    // Forget account credentials, cookies
    account->account()->credentials()->forgetSensitiveData();
    QFile::remove(account->account()->cookieJarPath());

    auto settings = ConfigFile::settingsWithGroup(QLatin1String(accountsC));
    settings->remove(account->account()->id());

    // Forget E2E keys
    account->account()->e2e()->forgetSensitiveData(account->account());

    emit accountSyncConnectionRemoved(account);
    emit accountRemoved(account);
}

AccountPtr AccountManager::createAccount()
{
    AccountPtr acc = Account::create();
    acc->setSslErrorHandler(new SslDialogErrorHandler);
    connect(acc.data(), &Account::proxyAuthenticationRequired,
        ProxyAuthHandler::instance(), &ProxyAuthHandler::handleProxyAuthenticationRequired);

    return acc;
}

void AccountManager::displayMnemonic(const QString& mnemonic)
{
    auto *widget = new QDialog;
    Ui_Dialog ui;
    ui.setupUi(widget);
    widget->setWindowTitle(tr("End to end encryption mnemonic"));
    ui.label->setText(tr("To protect your Cryptographic Identity, we encrypt it with a mnemonic of 12 dictionary words. "
                         "Please note these down and keep them safe. "
                         "They will be needed to add other devices to your account (like your mobile phone or laptop)."));
    ui.textEdit->setText(mnemonic);
    ui.textEdit->focusWidget();
    ui.textEdit->selectAll();
    ui.textEdit->setAlignment(Qt::AlignCenter);
    widget->exec();
    widget->resize(widget->sizeHint());
}

void AccountManager::shutdown()
{
    const auto accountsCopy = _accounts;
    _accounts.clear();
    for (const auto &acc : accountsCopy) {
        emit accountRemoved(acc.data());
        emit removeAccountFolders(acc.data());
    }
}

QList<AccountStatePtr> AccountManager::accounts() const
{
     return _accounts;
}

bool AccountManager::isAccountIdAvailable(const QString &id) const
{
    if (_additionalBlockedAccountIds.contains(id))
        return false;

    return std::none_of(_accounts.cbegin(), _accounts.cend(), [id](const auto &acc) {
        return acc->account()->id() == id;
    });
}

QString AccountManager::generateFreeAccountId() const
{
    int i = 0;
    forever {
        QString id = QString::number(i);
        if (isAccountIdAvailable(id)) {
            return id;
        }
        ++i;
    }
}

void AccountManager::addAccountState(AccountState *accountState)
{
    QObject::connect(accountState->account().data(),
        &Account::wantsAccountSaved,
        this, &AccountManager::saveAccount);

    AccountStatePtr ptr(accountState);
    _accounts << ptr;
    emit accountAdded(accountState);
}
}
