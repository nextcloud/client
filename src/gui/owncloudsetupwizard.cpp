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

#include <QAbstractButton>
#include <QtCore>
#include <QProcess>
#include <QMessageBox>
#include <QDesktopServices>
#include <QApplication>

#include "wizard/owncloudwizardcommon.h"
#include "wizard/owncloudwizard.h"
#include "owncloudsetupwizard.h"
#include "configfile.h"
#include "folderman.h"
#include "accessmanager.h"
#include "account.h"
#include "networkjobs.h"
#include "sslerrordialog.h"
#include "accountmanager.h"
#include "clientproxy.h"
#include "filesystem.h"
#include "owncloudgui.h"

#include "creds/credentialsfactory.h"
#include "creds/abstractcredentials.h"
#include "creds/dummycredentials.h"

namespace OCC {

OwncloudSetupWizard::OwncloudSetupWizard(QObject *parent)
    : QObject(parent)
    , _ocWizard(new OwncloudWizard)
    , _remoteFolder()
{
    connect(_ocWizard, &OwncloudWizard::determineAuthType,
        this, &OwncloudSetupWizard::slotCheckServer);
    connect(_ocWizard, &OwncloudWizard::connectToOCUrl,
        this, &OwncloudSetupWizard::slotConnectToOCUrl);
    connect(_ocWizard, &OwncloudWizard::createLocalAndRemoteFolders,
        this, &OwncloudSetupWizard::slotCreateLocalAndRemoteFolders);
    /* basicSetupFinished might be called from a reply from the network.
       slotAssistantFinished might destroy the temporary QNetworkAccessManager.
       Therefore Qt::QueuedConnection is required */
    connect(_ocWizard, &OwncloudWizard::basicSetupFinished,
        this, &OwncloudSetupWizard::slotAssistantFinished, Qt::QueuedConnection);
    connect(_ocWizard, &QDialog::finished, this, &QObject::deleteLater);
    connect(_ocWizard, &OwncloudWizard::skipFolderConfiguration, this, &OwncloudSetupWizard::slotSkipFolderConfiguration);
}

OwncloudSetupWizard::~OwncloudSetupWizard()
{
    _ocWizard->deleteLater();
}

static QPointer<OwncloudSetupWizard> wiz = nullptr;

void OwncloudSetupWizard::runWizard(QObject *obj, const char *amember, QWidget *parent)
{
    if (!wiz.isNull()) {
        bringWizardToFrontIfVisible();
        return;
    }

    wiz = new OwncloudSetupWizard(parent);
    connect(wiz, SIGNAL(ownCloudWizardDone(int)), obj, amember);
    FolderMan::instance()->setSyncEnabled(false);
    wiz->startWizard();
}

bool OwncloudSetupWizard::bringWizardToFrontIfVisible()
{
    if (wiz.isNull()) {
        return false;
    }

    ownCloudGui::raiseDialog(wiz->_ocWizard);
    return true;
}

void OwncloudSetupWizard::startWizard()
{
    AccountPtr account = AccountManager::createAccount();
    account->setCredentials(CredentialsFactory::create("dummy"));
    account->setUrl(Theme::instance()->overrideServerUrl());
    _ocWizard->setAccount(account);
    _ocWizard->setOCUrl(account->url().toString());

    _remoteFolder = Theme::instance()->defaultServerFolder();
    // remoteFolder may be empty, which means /
    QString localFolder = Theme::instance()->defaultClientFolder();

    // if its a relative path, prepend with users home dir, otherwise use as absolute path

    if (!QDir(localFolder).isAbsolute()) {
        localFolder = QDir::homePath() + QLatin1Char('/') + localFolder;
    }

    _ocWizard->setProperty("localFolder", localFolder);

    // remember the local folder to compare later if it changed, but clean first
    QString lf = QDir::fromNativeSeparators(localFolder);
    if (!lf.endsWith(QLatin1Char('/'))) {
        lf.append(QLatin1Char('/'));
    }

    _initLocalFolder = lf;

    _ocWizard->setRemoteFolder(_remoteFolder);

#ifdef WITH_PROVIDERS
    const auto startPage = WizardCommon::Page_Welcome;
#else // WITH_PROVIDERS
    const auto startPage = WizardCommon::Page_ServerSetup;
#endif // WITH_PROVIDERS
    _ocWizard->setStartId(startPage);

    _ocWizard->restart();

    _ocWizard->open();
    _ocWizard->raise();
}

// also checks if an installation is valid and determines auth type in a second step
void OwncloudSetupWizard::slotCheckServer(const QString &urlString)
{
    QString fixedUrl = urlString;
    QUrl url = QUrl::fromUserInput(fixedUrl);
    // fromUserInput defaults to http, not http if no scheme is specified
    if (!fixedUrl.startsWith("http://") && !fixedUrl.startsWith("https://")) {
        url.setScheme("https");
    }
    AccountPtr account = _ocWizard->account();
    account->setUrl(url);

    // Reset the proxy which might had been determined previously in ConnectionValidator::checkServerAndAuth()
    // when there was a previous account.
    account->networkAccessManager()->setProxy(QNetworkProxy(QNetworkProxy::NoProxy));

    // And also reset the QSslConfiguration, for the same reason (#6832)
    // Here the client certificate is added, if any. Later it'll be in HttpCredentials
    account->setSslConfiguration(QSslConfiguration());
    auto sslConfiguration = account->getOrCreateSslConfig(); // let Account set defaults
    if (!_ocWizard->_clientSslCertificate.isNull()) {
        sslConfiguration.setLocalCertificate(_ocWizard->_clientSslCertificate);
        sslConfiguration.setPrivateKey(_ocWizard->_clientSslKey);
    }
    // Be sure to merge the CAs
    auto ca = sslConfiguration.systemCaCertificates();
    ca.append(_ocWizard->_clientSslCaCertificates);
    sslConfiguration.setCaCertificates(ca);
    account->setSslConfiguration(sslConfiguration);

    // Make sure TCP connections get re-established
    account->networkAccessManager()->clearAccessCache();

    // Lookup system proxy in a thread https://github.com/owncloud/client/issues/2993
    if (ClientProxy::isUsingSystemDefault()) {
        qCDebug(lcWizard) << "Trying to look up system proxy";
        ClientProxy::lookupSystemProxyAsync(account->url(),
            this, SLOT(slotSystemProxyLookupDone(QNetworkProxy)));
    } else {
        // We want to reset the QNAM proxy so that the global proxy settings are used (via ClientProxy settings)
        account->networkAccessManager()->setProxy(QNetworkProxy(QNetworkProxy::DefaultProxy));
        // use a queued invocation so we're as asynchronous as with the other code path
        QMetaObject::invokeMethod(this, "slotFindServer", Qt::QueuedConnection);
    }
}

void OwncloudSetupWizard::slotSystemProxyLookupDone(const QNetworkProxy &proxy)
{
    if (proxy.type() != QNetworkProxy::NoProxy) {
        qCInfo(lcWizard) << "Setting QNAM proxy to be system proxy" << ClientProxy::printQNetworkProxy(proxy);
    } else {
        qCInfo(lcWizard) << "No system proxy set by OS";
    }
    AccountPtr account = _ocWizard->account();
    account->networkAccessManager()->setProxy(proxy);

    slotFindServer();
}

void OwncloudSetupWizard::slotFindServer()
{
    AccountPtr account = _ocWizard->account();

    // Set fake credentials before we check what credential it actually is.
    account->setCredentials(CredentialsFactory::create("dummy"));

    // Determining the actual server URL can be a multi-stage process
    // 1. Check url/status.php with CheckServerJob
    //    If that works we're done. In that case we don't check the
    //    url directly for redirects, see #5954.
    // 2. Check the url for permanent redirects (like url shorteners)
    // 3. Check redirected-url/status.php with CheckServerJob

    // Step 1: Check url/status.php
    auto *job = new CheckServerJob(account, this);
    job->setIgnoreCredentialFailure(true);
    connect(job, &CheckServerJob::instanceFound, this, &OwncloudSetupWizard::slotFoundServer);
    connect(job, &CheckServerJob::instanceNotFound, this, &OwncloudSetupWizard::slotFindServerBehindRedirect);
    connect(job, &CheckServerJob::timeout, this, &OwncloudSetupWizard::slotNoServerFoundTimeout);
    job->setTimeout((account->url().scheme() == "https") ? 30 * 1000 : 10 * 1000);
    job->start();

    // Step 2 and 3 are in slotFindServerBehindRedirect()
}

void OwncloudSetupWizard::slotFindServerBehindRedirect()
{
    AccountPtr account = _ocWizard->account();

    // Step 2: Resolve any permanent redirect chains on the base url
//    auto redirectCheckJob = account->sendRequest("GET", account->url());

//    // Use a significantly reduced timeout for this redirect check:
//    // the 5-minute default is inappropriate.
//    redirectCheckJob->setTimeout(qMin(2000ll, redirectCheckJob->timeoutMsec()));

//    // Grab the chain of permanent redirects and adjust the account url
//    // accordingly
//    auto permanentRedirects = std::make_shared<int>(0);
//    connect(redirectCheckJob, &AbstractNetworkJob::redirected, this,
//        [permanentRedirects, account](QNetworkReply *reply, const QUrl &targetUrl, int count) {
//            int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
//            if (count == *permanentRedirects && (httpCode == 301 || httpCode == 308)) {
//                qCInfo(lcWizard) << account->url() << " was redirected to" << targetUrl;
//                account->setUrl(targetUrl);
//                *permanentRedirects += 1;
//            }
//        });

//    // Step 3: When done, start checking status.php.
//    connect(redirectCheckJob, &SimpleNetworkJob::finishedSignal, this,
//        [this, account]() {
//            auto *job = new CheckServerJob(account, this);
//            job->setIgnoreCredentialFailure(true);
//            connect(job, &CheckServerJob::instanceFound, this, &OwncloudSetupWizard::slotFoundServer);
//            connect(job, &CheckServerJob::instanceNotFound, this, &OwncloudSetupWizard::slotNoServerFound);
//            connect(job, &CheckServerJob::timeout, this, &OwncloudSetupWizard::slotNoServerFoundTimeout);
//            job->setTimeout((account->url().scheme() == "https") ? 30 * 1000 : 10 * 1000);
//            job->start();
//    });
}

void OwncloudSetupWizard::slotFoundServer(const QUrl &url, const QJsonObject &info)
{
    auto serverVersion = CheckServerJob::version(info);

    _ocWizard->appendToConfigurationLog(tr("<font color=\"green\">Successfully connected to %1: %2 version %3 (%4)</font><br/><br/>")
                                            .arg(Utility::escape(url.toString()),
                                                Utility::escape(Theme::instance()->appNameGUI()),
                                                Utility::escape(CheckServerJob::versionString(info)),
                                                Utility::escape(serverVersion)));

    // Note with newer servers we get the version actually only later in capabilities
    // https://github.com/owncloud/core/pull/27473/files
    _ocWizard->account()->setServerVersion(serverVersion);

    if (url != _ocWizard->account()->url()) {
        // We might be redirected, update the account
        _ocWizard->account()->setUrl(url);
        qCInfo(lcWizard) << " was redirected to" << url.toString();
    }

    slotDetermineAuthType();
}

void OwncloudSetupWizard::slotNoServerFound(QNetworkReply *reply)
{
    auto job = qobject_cast<CheckServerJob *>(sender());

    // Do this early because reply might be deleted in message box event loop
    QString msg;
    if (!_ocWizard->account()->url().isValid()) {
        msg = tr("Invalid URL");
    } else {
        msg = tr("Failed to connect to %1 at %2:<br/>%3")
                  .arg(Utility::escape(Theme::instance()->appNameGUI()),
                      Utility::escape(_ocWizard->account()->url().toString()),
                      Utility::escape(job->errorString()));
    }
    bool isDowngradeAdvised = checkDowngradeAdvised(reply);

    // Displays message inside wizard and possibly also another message box
    _ocWizard->displayError(msg, isDowngradeAdvised);

    // Allow the credentials dialog to pop up again for the same URL.
    // Maybe the user just clicked 'Cancel' by accident or changed his mind.
    _ocWizard->account()->resetRejectedCertificates();
}

void OwncloudSetupWizard::slotNoServerFoundTimeout(const QUrl &url)
{
    _ocWizard->displayError(
        tr("Timeout while trying to connect to %1 at %2.")
            .arg(Utility::escape(Theme::instance()->appNameGUI()), Utility::escape(url.toString())),
                false);
}

void OwncloudSetupWizard::slotDetermineAuthType()
{
    auto *job = new DetermineAuthTypeJob(_ocWizard->account(), this);
    connect(job, &DetermineAuthTypeJob::authType,
        _ocWizard, &OwncloudWizard::setAuthType);
    job->start();
}

void OwncloudSetupWizard::slotConnectToOCUrl(const QString &url)
{
    qCInfo(lcWizard) << "Connect to url: " << url;
    AbstractCredentials *creds = _ocWizard->getCredentials();
    _ocWizard->account()->setCredentials(creds);
    _ocWizard->setField(QLatin1String("OCUrl"), url);
    _ocWizard->appendToConfigurationLog(tr("Trying to connect to %1 at %2 …")
                                            .arg(Theme::instance()->appNameGUI())
                                            .arg(url));

    testOwnCloudConnect();
}

void OwncloudSetupWizard::testOwnCloudConnect()
{
    AccountPtr account = _ocWizard->account();

    auto *job = new PropfindJob(account, "/", this);
    job->setIgnoreCredentialFailure(true);
    // There is custom redirect handling in the error handler,
    // so don't automatically follow redirects.
    job->setFollowRedirects(false);
    job->setProperties(QList<QByteArray>() << "getlastmodified");
    connect(job, &PropfindJob::result, _ocWizard, &OwncloudWizard::successfulStep);
    connect(job, &PropfindJob::finishedWithError, this, &OwncloudSetupWizard::slotAuthError);
    job->start();
}

void OwncloudSetupWizard::slotAuthError()
{
    QString errorMsg;

    auto *job = qobject_cast<PropfindJob *>(sender());
    if (!job) {
        qCWarning(lcWizard) << "Cannot check for authed redirects. This slot should be invoked from PropfindJob!";
        return;
    }
    QNetworkReply *reply = job->reply();

    // If there were redirects on the *authed* requests, also store
    // the updated server URL, similar to redirects on status.php.
    QUrl redirectUrl = reply->attribute(QNetworkRequest::RedirectionTargetAttribute).toUrl();
    if (!redirectUrl.isEmpty()) {
        qCInfo(lcWizard) << "Authed request was redirected to" << redirectUrl.toString();

        // strip the expected path
        QString path = redirectUrl.path();
        static QString expectedPath = "/" + _ocWizard->account()->davPath();
        if (path.endsWith(expectedPath)) {
            path.chop(expectedPath.size());
            redirectUrl.setPath(path);

            qCInfo(lcWizard) << "Setting account url to" << redirectUrl.toString();
            _ocWizard->account()->setUrl(redirectUrl);
            testOwnCloudConnect();
            return;
        }
        errorMsg = tr("The authenticated request to the server was redirected to "
                      "'%1'. The URL is bad, the server is misconfigured.")
                       .arg(Utility::escape(redirectUrl.toString()));

        // A 404 is actually a success: we were authorized to know that the folder does
        // not exist. It will be created later...
    } else if (reply->error() == QNetworkReply::ContentNotFoundError) {
        _ocWizard->successfulStep();
        return;

        // Provide messages for other errors, such as invalid credentials.
    } else if (reply->error() != QNetworkReply::NoError) {
        if (!_ocWizard->account()->credentials()->stillValid(reply)) {
            errorMsg = tr("Access forbidden by server. To verify that you have proper access, "
                          "<a href=\"%1\">click here</a> to access the service with your browser.")
                           .arg(Utility::escape(_ocWizard->account()->url().toString()));
        } else {
            errorMsg = job->errorStringParsingBody();
        }

        // Something else went wrong, maybe the response was 200 but with invalid data.
    } else {
        errorMsg = tr("There was an invalid response to an authenticated WebDAV request");
    }

    // bring wizard to top
    _ocWizard->bringToTop();
    if (_ocWizard->currentId() == WizardCommon::Page_OAuthCreds || _ocWizard->currentId() == WizardCommon::Page_Flow2AuthCreds) {
        _ocWizard->back();
    }
    _ocWizard->displayError(errorMsg, _ocWizard->currentId() == WizardCommon::Page_ServerSetup && checkDowngradeAdvised(reply));
}

bool OwncloudSetupWizard::checkDowngradeAdvised(QNetworkReply *reply)
{
    if (reply->url().scheme() != QLatin1String("https")) {
        return false;
    }

    switch (reply->error()) {
    case QNetworkReply::NoError:
    case QNetworkReply::ContentNotFoundError:
    case QNetworkReply::AuthenticationRequiredError:
    case QNetworkReply::HostNotFoundError:
        return false;
    default:
        break;
    }

    // Adhere to HSTS, even though we do not parse it properly
    if (reply->hasRawHeader("Strict-Transport-Security")) {
        return false;
    }
    return true;
}

void OwncloudSetupWizard::slotCreateLocalAndRemoteFolders(const QString &localFolder, const QString &remoteFolder)
{
    qCInfo(lcWizard) << "Setup local sync folder for new oC connection " << localFolder;
    const QDir fi(localFolder);

    bool nextStep = true;
    if (fi.exists()) {
        FileSystem::setFolderMinimumPermissions(localFolder);
        Utility::setupFavLink(localFolder);
        // there is an existing local folder. If its non empty, it can only be synced if the
        // ownCloud is newly created.
        _ocWizard->appendToConfigurationLog(
            tr("Local sync folder %1 already exists, setting it up for sync.<br/><br/>")
                .arg(Utility::escape(localFolder)));
    } else {
        QString res = tr("Creating local sync folder %1 …").arg(localFolder);
        if (fi.mkpath(localFolder)) {
            FileSystem::setFolderMinimumPermissions(localFolder);
            Utility::setupFavLink(localFolder);
            res += tr("OK");
        } else {
            res += tr("failed.");
            qCWarning(lcWizard) << "Failed to create " << fi.path();
            _ocWizard->displayError(tr("Could not create local folder %1").arg(Utility::escape(localFolder)), false);
            nextStep = false;
        }
        _ocWizard->appendToConfigurationLog(res);
    }
    if (nextStep) {
        /*
         * BEGIN - Sanitize URL paths to eliminate double-slashes
         *
         *         Purpose: Don't rely on unsafe paths, be extra careful.
         *
         *         Example: https://cloud.example.com/remote.php/webdav//
         *
        */
        qCInfo(lcWizard) << "Sanitize got URL path:" << QString(_ocWizard->account()->url().toString() + '/' + _ocWizard->account()->davPath() + remoteFolder);

        QString newDavPath = _ocWizard->account()->davPath(),
                newRemoteFolder = remoteFolder;

        while (newDavPath.startsWith('/')) {
            newDavPath.remove(0, 1);
        }
        while (newDavPath.endsWith('/')) {
            newDavPath.chop(1);
        }

        while (newRemoteFolder.startsWith('/')) {
            newRemoteFolder.remove(0, 1);
        }
        while (newRemoteFolder.endsWith('/')) {
            newRemoteFolder.chop(1);
        }

        QString newUrlPath = newDavPath + '/' + newRemoteFolder;

        qCInfo(lcWizard) << "Sanitized to URL path:" << _ocWizard->account()->url().toString() + '/' + newUrlPath;
        /*
         * END - Sanitize URL paths to eliminate double-slashes
        */

        auto *job = new EntityExistsJob(_ocWizard->account(), newUrlPath, this);
        connect(job, &EntityExistsJob::exists, this, &OwncloudSetupWizard::slotRemoteFolderExists);
        job->start();
    } else {
        finalizeSetup(false);
    }
}

// ### TODO move into EntityExistsJob once we decide if/how to return gui strings from jobs
void OwncloudSetupWizard::slotRemoteFolderExists(QNetworkReply *reply)
{
    auto job = qobject_cast<EntityExistsJob *>(sender());
    bool ok = true;
    QString error;
    QNetworkReply::NetworkError errId = reply->error();

    if (errId == QNetworkReply::NoError) {
        qCInfo(lcWizard) << "Remote folder found, all cool!";
    } else if (errId == QNetworkReply::ContentNotFoundError) {
        if (_remoteFolder.isEmpty()) {
            error = tr("No remote folder specified!");
            ok = false;
        } else {
            createRemoteFolder();
        }
    } else {
        error = tr("Error: %1").arg(job->errorString());
        ok = false;
    }

    if (!ok) {
        _ocWizard->displayError(Utility::escape(error), false);
    }

    finalizeSetup(ok);
}

void OwncloudSetupWizard::createRemoteFolder()
{
    _ocWizard->appendToConfigurationLog(tr("creating folder on Nextcloud: %1").arg(_remoteFolder));

    auto *job = new MkColJob(_ocWizard->account(), _remoteFolder, this);
    connect(job, SIGNAL(finished(QNetworkReply::NetworkError)), SLOT(slotCreateRemoteFolderFinished(QNetworkReply::NetworkError)));
    job->start();
}

void OwncloudSetupWizard::slotCreateRemoteFolderFinished(QNetworkReply::NetworkError error)
{
    qCDebug(lcWizard) << "** webdav mkdir request finished " << error;
    //    disconnect(ownCloudInfo::instance(), SIGNAL(webdavColCreated(QNetworkReply::NetworkError)),
    //               this, SLOT(slotCreateRemoteFolderFinished(QNetworkReply::NetworkError)));

    bool success = true;

    if (error == QNetworkReply::NoError) {
        _ocWizard->appendToConfigurationLog(tr("Remote folder %1 created successfully.").arg(_remoteFolder));
    } else if (error == 202) {
        _ocWizard->appendToConfigurationLog(tr("The remote folder %1 already exists. Connecting it for syncing.").arg(_remoteFolder));
    } else if (error > 202 && error < 300) {
        _ocWizard->displayError(tr("The folder creation resulted in HTTP error code %1").arg((int)error), false);

        _ocWizard->appendToConfigurationLog(tr("The folder creation resulted in HTTP error code %1").arg((int)error));
    } else if (error == QNetworkReply::OperationCanceledError) {
        _ocWizard->displayError(tr("The remote folder creation failed because the provided credentials "
                                   "are wrong!"
                                   "<br/>Please go back and check your credentials.</p>"),
            false);
        _ocWizard->appendToConfigurationLog(tr("<p><font color=\"red\">Remote folder creation failed probably because the provided credentials are wrong.</font>"
                                               "<br/>Please go back and check your credentials.</p>"));
        _remoteFolder.clear();
        success = false;
    } else {
        _ocWizard->appendToConfigurationLog(tr("Remote folder %1 creation failed with error <tt>%2</tt>.").arg(Utility::escape(_remoteFolder)).arg(error));
        _ocWizard->displayError(tr("Remote folder %1 creation failed with error <tt>%2</tt>.").arg(Utility::escape(_remoteFolder)).arg(error), false);
        _remoteFolder.clear();
        success = false;
    }

    finalizeSetup(success);
}

void OwncloudSetupWizard::finalizeSetup(bool success)
{
    // enable/disable the finish button.
    _ocWizard->enableFinishOnResultWidget(success);

    const QString localFolder = _ocWizard->property("localFolder").toString();
    if (success) {
        if (!(localFolder.isEmpty() || _remoteFolder.isEmpty())) {
            _ocWizard->appendToConfigurationLog(
                tr("A sync connection from %1 to remote directory %2 was set up.")
                    .arg(localFolder, _remoteFolder));
        }
        _ocWizard->appendToConfigurationLog(QLatin1String(" "));
        _ocWizard->appendToConfigurationLog(QLatin1String("<p><font color=\"green\"><b>")
            + tr("Successfully connected to %1!")
                  .arg(Theme::instance()->appNameGUI())
            + QLatin1String("</b></font></p>"));
        _ocWizard->successfulStep();
    } else {
        // ### this is not quite true, pass in the real problem as optional parameter
        _ocWizard->appendToConfigurationLog(QLatin1String("<p><font color=\"red\">")
            + tr("Connection to %1 could not be established. Please check again.")
                  .arg(Theme::instance()->appNameGUI())
            + QLatin1String("</font></p>"));
    }
}

bool OwncloudSetupWizard::ensureStartFromScratch(const QString &localFolder)
{
    // first try to rename (backup) the current local dir.
    bool renameOk = false;
    while (!renameOk) {
        renameOk = FolderMan::instance()->startFromScratch(localFolder);
        if (!renameOk) {
            QMessageBox::StandardButton but = QMessageBox::question(nullptr, tr("Folder rename failed"),
                tr("Cannot remove and back up the folder because the folder or a file in it is open in another program."
                   " Please close the folder or file and hit retry or cancel the setup."),
                QMessageBox::Retry | QMessageBox::Abort, QMessageBox::Retry);
            if (but == QMessageBox::Abort) {
                break;
            }
        }
    }
    return renameOk;
}

// Method executed when the user end has finished the basic setup.
void OwncloudSetupWizard::slotAssistantFinished(int result)
{
    FolderMan *folderMan = FolderMan::instance();

    if (result == QDialog::Rejected) {
        qCInfo(lcWizard) << "Rejected the new config, use the old!";

    } else if (result == QDialog::Accepted) {
        // This may or may not wipe all folder definitions, depending
        // on whether a new account is activated or the existing one
        // is changed.
        auto account = applyAccountChanges();

        QString localFolder = FolderDefinition::prepareLocalPath(_ocWizard->localFolder());

        bool startFromScratch = _ocWizard->field("OCSyncFromScratch").toBool();
        if (!startFromScratch || ensureStartFromScratch(localFolder)) {
            qCInfo(lcWizard) << "Adding folder definition for" << localFolder << _remoteFolder;
            FolderDefinition folderDefinition;
            folderDefinition.localPath = localFolder;
            folderDefinition.targetPath = FolderDefinition::prepareTargetPath(_remoteFolder);
            folderDefinition.ignoreHiddenFiles = folderMan->ignoreHiddenFiles();
            if (_ocWizard->useVirtualFileSync()) {
                folderDefinition.virtualFilesMode = bestAvailableVfsMode();
            }
            if (folderMan->navigationPaneHelper().showInExplorerNavigationPane())
                folderDefinition.navigationPaneClsid = QUuid::createUuid();

            auto f = folderMan->addFolder(account, folderDefinition);
            if (f) {
                if (folderDefinition.virtualFilesMode != Vfs::Off && _ocWizard->useVirtualFileSync())
                    f->setRootPinState(PinState::OnlineOnly);

                f->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList,
                    _ocWizard->selectiveSyncBlacklist());
                if (!_ocWizard->isConfirmBigFolderChecked()) {
                    // The user already accepted the selective sync dialog. everything is in the white list
                    f->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncWhiteList,
                        QStringList() << QLatin1String("/"));
                }
            }
            _ocWizard->appendToConfigurationLog(tr("<font color=\"green\"><b>Local sync folder %1 successfully created!</b></font>").arg(localFolder));
        }
    }

    // notify others.
    emit ownCloudWizardDone(result);
}

void OwncloudSetupWizard::slotSkipFolderConfiguration()
{
    applyAccountChanges();

    disconnect(_ocWizard, &OwncloudWizard::basicSetupFinished,
        this, &OwncloudSetupWizard::slotAssistantFinished);
    _ocWizard->close();
    emit ownCloudWizardDone(QDialog::Accepted);
}

AccountState *OwncloudSetupWizard::applyAccountChanges()
{
    AccountPtr newAccount = _ocWizard->account();

    // Detach the account that is going to be saved from the
    // wizard to ensure it doesn't accidentally get modified
    // later (such as from running cleanup such as
    // AbstractCredentialsWizardPage::cleanupPage())
    _ocWizard->setAccount(AccountManager::createAccount());

    auto manager = AccountManager::instance();

    auto newState = manager->addAccount(newAccount);
    manager->save();
    return newState;
}

} // namespace OCC
