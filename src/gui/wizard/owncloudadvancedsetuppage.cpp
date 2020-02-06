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

#include <QDir>
#include <QFileDialog>
#include <QUrl>
#include <QTimer>
#include <QStorageInfo>

#include "QProgressIndicator.h"

#include "wizard/owncloudwizard.h"
#include "wizard/owncloudwizardcommon.h"
#include "wizard/owncloudadvancedsetuppage.h"
#include "account.h"
#include "theme.h"
#include "configfile.h"
#include "selectivesyncdialog.h"
#include <folderman.h>
#include "creds/abstractcredentials.h"
#include "networkjobs.h"

namespace OCC {

OwncloudAdvancedSetupPage::OwncloudAdvancedSetupPage()
    : QWizardPage()
    , _ui()
    , _checking(false)
    , _created(false)
    , _localFolderValid(false)
    , _progressIndi(new QProgressIndicator(this))
    , _remoteFolder()
    , _rSize(-1)
    , _rSelectedSize(-1)
{
    _ui.setupUi(this);

    Theme *theme = Theme::instance();
    setTitle(WizardCommon::titleTemplate().arg(tr("Connect to %1").arg(theme->appNameGUI())));
    setSubTitle(WizardCommon::subTitleTemplate().arg(tr("Setup local folder options")));

    registerField(QLatin1String("OCSyncFromScratch"), _ui.cbSyncFromScratch);

    _ui.resultLayout->addWidget(_progressIndi);
    stopSpinner();
    setupCustomization();

    connect(_ui.pbSelectLocalFolder, &QAbstractButton::clicked, this, &OwncloudAdvancedSetupPage::slotSelectFolder);
    setButtonText(QWizard::NextButton, tr("Connect …"));

    connect(_ui.rSyncEverything, &QAbstractButton::clicked, this, &OwncloudAdvancedSetupPage::slotSyncEverythingClicked);
    connect(_ui.rSelectiveSync, &QAbstractButton::clicked, this, &OwncloudAdvancedSetupPage::slotSelectiveSyncClicked);
    connect(_ui.bSelectiveSync, &QAbstractButton::clicked, this, &OwncloudAdvancedSetupPage::slotSelectiveSyncClicked);

    QIcon appIcon = theme->applicationIcon();
    _ui.lServerIcon->setText(QString());
    _ui.lServerIcon->setPixmap(appIcon.pixmap(48));
    _ui.lLocalIcon->setText(QString());
    _ui.lLocalIcon->setPixmap(QPixmap(Theme::hidpiFileName(":/client/resources/folder-sync.png")));

    if (theme->wizardHideExternalStorageConfirmationCheckbox()) {
        _ui.confCheckBoxExternal->hide();
    }
    if (theme->wizardHideFolderSizeLimitCheckbox()) {
        _ui.confCheckBoxSize->hide();
        _ui.confSpinBox->hide();
        _ui.confTraillingSizeLabel->hide();
    }
}

void OwncloudAdvancedSetupPage::setupCustomization()
{
    // set defaults for the customize labels.
    _ui.topLabel->hide();
    _ui.bottomLabel->hide();

    Theme *theme = Theme::instance();
    QVariant variant = theme->customMedia(Theme::oCSetupTop);
    if (!variant.isNull()) {
        WizardCommon::setupCustomMedia(variant, _ui.topLabel);
    }

    variant = theme->customMedia(Theme::oCSetupBottom);
    WizardCommon::setupCustomMedia(variant, _ui.bottomLabel);
}

bool OwncloudAdvancedSetupPage::isComplete() const
{
    return !_checking && _localFolderValid;
}

void OwncloudAdvancedSetupPage::initializePage()
{
    WizardCommon::initErrorLabel(_ui.errorLabel);

    _checking = false;
    _ui.lSelectiveSyncSizeLabel->setText(QString());
    _ui.lSyncEverythingSizeLabel->setText(QString());

    // Update the local folder - this is not guaranteed to find a good one
    QString goodLocalFolder = FolderMan::instance()->findGoodPathForNewSyncFolder(localFolder(), serverUrl());
    wizard()->setProperty("localFolder", goodLocalFolder);

    // call to init label
    updateStatus();

    // ensure "next" gets the focus, not obSelectLocalFolder
    QTimer::singleShot(0, wizard()->button(QWizard::NextButton), SLOT(setFocus()));

    auto acc = static_cast<OwncloudWizard *>(wizard())->account();
    auto quotaJob = new PropfindJob(acc, _remoteFolder, this);
    quotaJob->setProperties(QList<QByteArray>() << "http://owncloud.org/ns:size");

    connect(quotaJob, &PropfindJob::result, this, &OwncloudAdvancedSetupPage::slotQuotaRetrieved);
    quotaJob->start();


    if (Theme::instance()->wizardSelectiveSyncDefaultNothing()) {
        _selectiveSyncBlacklist = QStringList("/");
        QTimer::singleShot(0, this, &OwncloudAdvancedSetupPage::slotSelectiveSyncClicked);
    }

    ConfigFile cfgFile;
    auto newSizeLimit = cfgFile.newBigFolderSizeLimit();
    _ui.confCheckBoxSize->setChecked(newSizeLimit.first);
    _ui.confSpinBox->setValue(newSizeLimit.second);
    _ui.confCheckBoxExternal->setChecked(cfgFile.confirmExternalStorage());
}

// Called if the user changes the user- or url field. Adjust the texts and
// evtl. warnings on the dialog.
void OwncloudAdvancedSetupPage::updateStatus()
{
    const QString locFolder = localFolder();

    // check if the local folder exists. If so, and if its not empty, show a warning.
    QString errorStr = FolderMan::instance()->checkPathValidityForNewFolder(locFolder, serverUrl());
    _localFolderValid = errorStr.isEmpty();

    QString t;

    _ui.pbSelectLocalFolder->setText(QDir::toNativeSeparators(locFolder));
    if (dataChanged()) {
        if (_remoteFolder.isEmpty() || _remoteFolder == QLatin1String("/")) {
            t = "";
        } else {
            t = Utility::escape(tr("%1 folder '%2' is synced to local folder '%3'")
                                    .arg(Theme::instance()->appName(), _remoteFolder,
                                        QDir::toNativeSeparators(locFolder)));
            _ui.rSyncEverything->setText(tr("Sync the folder '%1'").arg(_remoteFolder));
        }

        const bool dirNotEmpty(QDir(locFolder).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).count() > 0);
        if (dirNotEmpty) {
            t += tr("<p><small><strong>Warning:</strong> The local folder is not empty. "
                    "Pick a resolution!</small></p>");
        }
        _ui.resolutionWidget->setVisible(dirNotEmpty);
    } else {
        _ui.resolutionWidget->setVisible(false);
    }

    QString lfreeSpaceStr = Utility::octetsToString(availableLocalSpace());
    _ui.lFreeSpace->setText(QString(tr("Free space: %1")).arg(lfreeSpaceStr));

    _ui.syncModeLabel->setText(t);
    _ui.syncModeLabel->setFixedHeight(_ui.syncModeLabel->sizeHint().height());
    wizard()->resize(wizard()->sizeHint());

    qint64 rSpace = _ui.rSyncEverything->isChecked() ? _rSize : _rSelectedSize;

    QString spaceError = checkLocalSpace(rSpace);
    if (!spaceError.isEmpty()) {
        errorStr = spaceError;
    }
    setErrorString(errorStr);

    emit completeChanged();
}

/* obsolete */
bool OwncloudAdvancedSetupPage::dataChanged()
{
    return true;
}

void OwncloudAdvancedSetupPage::startSpinner()
{
    _ui.resultLayout->setEnabled(true);
    _progressIndi->setVisible(true);
    _progressIndi->startAnimation();
}

void OwncloudAdvancedSetupPage::stopSpinner()
{
    _ui.resultLayout->setEnabled(false);
    _progressIndi->setVisible(false);
    _progressIndi->stopAnimation();
}

QUrl OwncloudAdvancedSetupPage::serverUrl() const
{
    const QString urlString = static_cast<OwncloudWizard *>(wizard())->ocUrl();
    const QString user = static_cast<OwncloudWizard *>(wizard())->getCredentials()->user();

    QUrl url(urlString);
    url.setUserName(user);
    return url;
}

int OwncloudAdvancedSetupPage::nextId() const
{
    return WizardCommon::Page_Result;
}

QString OwncloudAdvancedSetupPage::localFolder() const
{
    QString folder = wizard()->property("localFolder").toString();
    return folder;
}

QStringList OwncloudAdvancedSetupPage::selectiveSyncBlacklist() const
{
    return _selectiveSyncBlacklist;
}

bool OwncloudAdvancedSetupPage::isConfirmBigFolderChecked() const
{
    return _ui.rSyncEverything->isChecked() && _ui.confCheckBoxSize->isChecked();
}

bool OwncloudAdvancedSetupPage::validatePage()
{
    if (!_created) {
        setErrorString(QString());
        _checking = true;
        startSpinner();
        emit completeChanged();

        if (_ui.rSyncEverything->isChecked()) {
            ConfigFile cfgFile;
            cfgFile.setNewBigFolderSizeLimit(_ui.confCheckBoxSize->isChecked(),
                _ui.confSpinBox->value());
            cfgFile.setConfirmExternalStorage(_ui.confCheckBoxExternal->isChecked());
        }

        emit createLocalAndRemoteFolders(localFolder(), _remoteFolder);
        return false;
    } else {
        // connecting is running
        _checking = false;
        emit completeChanged();
        stopSpinner();
        return true;
    }
}

void OwncloudAdvancedSetupPage::setErrorString(const QString &err)
{
    if (err.isEmpty()) {
        _ui.errorLabel->setVisible(false);
    } else {
        _ui.errorLabel->setVisible(true);
        _ui.errorLabel->setText(err);
    }
    _checking = false;
    emit completeChanged();
}

void OwncloudAdvancedSetupPage::directoriesCreated()
{
    _checking = false;
    _created = true;
    stopSpinner();
    emit completeChanged();
}

void OwncloudAdvancedSetupPage::setRemoteFolder(const QString &remoteFolder)
{
    if (!remoteFolder.isEmpty()) {
        _remoteFolder = remoteFolder;
    }
}

void OwncloudAdvancedSetupPage::slotSelectFolder()
{
    QString dir = QFileDialog::getExistingDirectory(nullptr, tr("Local Sync Folder"), QDir::homePath());
    if (!dir.isEmpty()) {
        _ui.pbSelectLocalFolder->setText(dir);
        wizard()->setProperty("localFolder", dir);
        updateStatus();
    }

    qint64 rSpace = _ui.rSyncEverything->isChecked() ? _rSize : _rSelectedSize;
    QString errorStr = checkLocalSpace(rSpace);
    setErrorString(errorStr);
}

void OwncloudAdvancedSetupPage::slotSelectiveSyncClicked()
{
    // Because clicking on it also changes it, restore it to the previous state in case the user cancelled the dialog
    _ui.rSyncEverything->setChecked(_selectiveSyncBlacklist.isEmpty());

    AccountPtr acc = static_cast<OwncloudWizard *>(wizard())->account();
    SelectiveSyncDialog *dlg = new SelectiveSyncDialog(acc, _remoteFolder, _selectiveSyncBlacklist, this);

    const int result = dlg->exec();
    bool updateBlacklist = false;

    // We need to update the selective sync blacklist either when the dialog
    // was accepted, or when it was used in conjunction with the
    // wizardSelectiveSyncDefaultNothing feature and was cancelled - in that
    // case the stub blacklist of / was expanded to the actual list of top
    // level folders by the selective sync dialog.
    if (result == QDialog::Accepted) {
        _selectiveSyncBlacklist = dlg->createBlackList();
        updateBlacklist = true;
    } else if (result == QDialog::Rejected && _selectiveSyncBlacklist == QStringList("/")) {
        _selectiveSyncBlacklist = dlg->oldBlackList();
        updateBlacklist = true;
    }

    if (updateBlacklist) {
        if (!_selectiveSyncBlacklist.isEmpty()) {
            _ui.rSelectiveSync->blockSignals(true);
            _ui.rSelectiveSync->setChecked(true);
            _ui.rSelectiveSync->blockSignals(false);
            auto s = dlg->estimatedSize();
            if (s > 0) {
                _ui.lSelectiveSyncSizeLabel->setText(tr("(%1)").arg(Utility::octetsToString(s)));

                _rSelectedSize = s;
                QString errorStr = checkLocalSpace(_rSelectedSize);
                setErrorString(errorStr);

            } else {
                _ui.lSelectiveSyncSizeLabel->setText(QString());
            }
        } else {
            _ui.rSyncEverything->setChecked(true);
            _ui.lSelectiveSyncSizeLabel->setText(QString());
        }
        wizard()->setProperty("blacklist", _selectiveSyncBlacklist);
    }
}

void OwncloudAdvancedSetupPage::slotSyncEverythingClicked()
{
    _ui.lSelectiveSyncSizeLabel->setText(QString());
    _ui.rSyncEverything->setChecked(true);
    _selectiveSyncBlacklist.clear();

    QString errorStr = checkLocalSpace(_rSize);
    setErrorString(errorStr);
}

void OwncloudAdvancedSetupPage::slotQuotaRetrieved(const QVariantMap &result)
{
    _rSize = result["size"].toDouble();
    _ui.lSyncEverythingSizeLabel->setText(tr("(%1)").arg(Utility::octetsToString(_rSize)));

    updateStatus();
}

qint64 OwncloudAdvancedSetupPage::availableLocalSpace() const
{
    QString localDir = localFolder();
    QString path = !QDir(localDir).exists() && localDir.contains(QDir::homePath()) ?
                QDir::homePath() : localDir;
    QStorageInfo storage(QDir::toNativeSeparators(path));

    return storage.bytesAvailable();
}

QString OwncloudAdvancedSetupPage::checkLocalSpace(qint64 remoteSize) const
{
    return (availableLocalSpace()>remoteSize) ? QString() : tr("There isn't enough free space in the local folder!");
}

void OwncloudAdvancedSetupPage::slotStyleChanged()
{
    customizeStyle();
}

void OwncloudAdvancedSetupPage::customizeStyle()
{
    if(_progressIndi)
        _progressIndi->setColor(QGuiApplication::palette().color(QPalette::Text));
}

} // namespace OCC
