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

#include "networksettings.h"
#include "ui_networksettings.h"

#include "theme.h"
#include "configfile.h"
#include "application.h"
#include "configfile.h"
#include "folderman.h"
#include "accountmanager.h"
#include "wifilistmanager.h"

#include <QNetworkProxy>
#include <QString>
#include <QList>

namespace OCC {

NetworkSettings::NetworkSettings(QWidget *parent)
    : QWidget(parent)
    , _ui(new Ui::NetworkSettings)
{
    _ui->setupUi(this);

    _ui->hostLineEdit->setPlaceholderText(tr("Hostname of proxy server"));
    _ui->userLineEdit->setPlaceholderText(tr("Username for proxy server"));
    _ui->passwordLineEdit->setPlaceholderText(tr("Password for proxy server"));

    _ui->typeComboBox->addItem(tr("HTTP(S) proxy"), QNetworkProxy::HttpProxy);
    _ui->typeComboBox->addItem(tr("SOCKS5 proxy"), QNetworkProxy::Socks5Proxy);

    _ui->authRequiredcheckBox->setEnabled(true);

    // Explicitly set up the enabled status of the proxy auth widgets to ensure
    // toggling the parent enables/disables the children
    _ui->userLineEdit->setEnabled(true);
    _ui->passwordLineEdit->setEnabled(true);
    _ui->authWidgets->setEnabled(_ui->authRequiredcheckBox->isChecked());
    connect(_ui->authRequiredcheckBox, &QAbstractButton::toggled,
        _ui->authWidgets, &QWidget::setEnabled);

    connect(_ui->manualProxyRadioButton, &QAbstractButton::toggled,
        _ui->manualSettings, &QWidget::setEnabled);
    connect(_ui->manualProxyRadioButton, &QAbstractButton::toggled,
        _ui->typeComboBox, &QWidget::setEnabled);

    loadProxySettings();
    loadBWLimitSettings();
    loadWifiListSettings();

    // proxy
    connect(_ui->typeComboBox, static_cast<void (QComboBox::*)(int)>(&QComboBox::currentIndexChanged), this, &NetworkSettings::saveProxySettings);
    connect(_ui->proxyButtonGroup, static_cast<void (QButtonGroup::*)(int)>(&QButtonGroup::buttonClicked), this, &NetworkSettings::saveProxySettings);
    connect(_ui->hostLineEdit, &QLineEdit::editingFinished, this, &NetworkSettings::saveProxySettings);
    connect(_ui->userLineEdit, &QLineEdit::editingFinished, this, &NetworkSettings::saveProxySettings);
    connect(_ui->passwordLineEdit, &QLineEdit::editingFinished, this, &NetworkSettings::saveProxySettings);
    connect(_ui->portSpinBox, &QAbstractSpinBox::editingFinished, this, &NetworkSettings::saveProxySettings);
    connect(_ui->authRequiredcheckBox, &QAbstractButton::toggled, this, &NetworkSettings::saveProxySettings);

    connect(_ui->uploadLimitRadioButton, &QAbstractButton::clicked, this, &NetworkSettings::saveBWLimitSettings);
    connect(_ui->noUploadLimitRadioButton, &QAbstractButton::clicked, this, &NetworkSettings::saveBWLimitSettings);
    connect(_ui->autoUploadLimitRadioButton, &QAbstractButton::clicked, this, &NetworkSettings::saveBWLimitSettings);
    connect(_ui->downloadLimitRadioButton, &QAbstractButton::clicked, this, &NetworkSettings::saveBWLimitSettings);
    connect(_ui->noDownloadLimitRadioButton, &QAbstractButton::clicked, this, &NetworkSettings::saveBWLimitSettings);
    connect(_ui->autoDownloadLimitRadioButton, &QAbstractButton::clicked, this, &NetworkSettings::saveBWLimitSettings);
    connect(_ui->downloadSpinBox, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, &NetworkSettings::saveBWLimitSettings);
    connect(_ui->uploadSpinBox, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, &NetworkSettings::saveBWLimitSettings);

    // Warn about empty proxy host
    connect(_ui->hostLineEdit, &QLineEdit::textChanged, this, &NetworkSettings::checkEmptyProxyHost);
    checkEmptyProxyHost();

    //Wi-Fi
    connect(_ui->activateWifiListsCheckbox, &QCheckBox::stateChanged, this, &NetworkSettings::wifiListsToggled);
    connect(_ui->manageWifiListsBtn, &QPushButton::clicked, [this]() { 
		WifiListManager *wifiMgr = new WifiListManager;
		wifiMgr->exec();
		_ui->wifiListsStatusLabel->setText(getActiveSsidsDescription()); });
}

NetworkSettings::~NetworkSettings()
{
    delete _ui;
}

QSize NetworkSettings::sizeHint() const
{
    return QSize(ownCloudGui::settingsDialogSize().width(), QWidget::sizeHint().height());
}

void NetworkSettings::wifiListsToggled()
{
    bool wifiListsActive = _ui->activateWifiListsCheckbox->checkState();
    _ui->manageWifiListsBtn->setEnabled(wifiListsActive);
    _ui->wifiListsStatusLabel->setEnabled(wifiListsActive);
    _ui->wifiListsStatusLabel->setText(wifiListsActive ? getActiveSsidsDescription() : QString("No Wi-Fi limitations active"));

    ConfigFile cfg;
    cfg.setWifiListActive(wifiListsActive);
}

QString NetworkSettings::getActiveSsidsDescription()
{
    OCC::ConfigFile cfgFile;
    QString desc = QString::number(cfgFile.ssidList().count()) + " SSIDs configured ";
    desc.append("(" + cfgFile.wifiListMode() + ")");
    return desc;
}

void NetworkSettings::loadProxySettings()
{
    if (Theme::instance()->forceSystemNetworkProxy()) {
        _ui->systemProxyRadioButton->setChecked(true);
        _ui->proxyGroupBox->setEnabled(false);
        return;
    }
    // load current proxy settings
    OCC::ConfigFile cfgFile;
    int type = cfgFile.proxyType();
    switch (type) {
    case QNetworkProxy::NoProxy:
        _ui->noProxyRadioButton->setChecked(true);
        break;
    case QNetworkProxy::DefaultProxy:
        _ui->systemProxyRadioButton->setChecked(true);
        break;
    case QNetworkProxy::Socks5Proxy:
    case QNetworkProxy::HttpProxy:
        _ui->typeComboBox->setCurrentIndex(_ui->typeComboBox->findData(type));
        _ui->manualProxyRadioButton->setChecked(true);
        break;
    default:
        break;
    }

    _ui->hostLineEdit->setText(cfgFile.proxyHostName());
    int port = cfgFile.proxyPort();
    if (port == 0)
        port = 8080;
    _ui->portSpinBox->setValue(port);
    _ui->authRequiredcheckBox->setChecked(cfgFile.proxyNeedsAuth());
    _ui->userLineEdit->setText(cfgFile.proxyUser());
    _ui->passwordLineEdit->setText(cfgFile.proxyPassword());
}

void NetworkSettings::loadBWLimitSettings()
{
    ConfigFile cfgFile;

    int useDownloadLimit = cfgFile.useDownloadLimit();
    if (useDownloadLimit >= 1) {
        _ui->downloadLimitRadioButton->setChecked(true);
    } else if (useDownloadLimit == 0) {
        _ui->noDownloadLimitRadioButton->setChecked(true);
    } else {
        _ui->autoDownloadLimitRadioButton->setChecked(true);
    }
    _ui->downloadSpinBox->setValue(cfgFile.downloadLimit());

    int useUploadLimit = cfgFile.useUploadLimit();
    if (useUploadLimit >= 1) {
        _ui->uploadLimitRadioButton->setChecked(true);
    } else if (useUploadLimit == 0) {
        _ui->noUploadLimitRadioButton->setChecked(true);
    } else {
        _ui->autoUploadLimitRadioButton->setChecked(true);
    }
    _ui->uploadSpinBox->setValue(cfgFile.uploadLimit());
}

void NetworkSettings::loadWifiListSettings()
{
    ConfigFile cfgFile;
    _ui->activateWifiListsCheckbox->setChecked(cfgFile.wifiListActive());
    _ui->manageWifiListsBtn->setEnabled(cfgFile.wifiListActive());
    _ui->wifiListsStatusLabel->setText(getActiveSsidsDescription());
}

void NetworkSettings::saveProxySettings()
{
    ConfigFile cfgFile;

    checkEmptyProxyHost();
    if (_ui->noProxyRadioButton->isChecked()) {
        cfgFile.setProxyType(QNetworkProxy::NoProxy);
    } else if (_ui->systemProxyRadioButton->isChecked()) {
        cfgFile.setProxyType(QNetworkProxy::DefaultProxy);
    } else if (_ui->manualProxyRadioButton->isChecked()) {
        int type = _ui->typeComboBox->itemData(_ui->typeComboBox->currentIndex()).toInt();
        QString host = _ui->hostLineEdit->text();
        if (host.isEmpty())
            type = QNetworkProxy::NoProxy;
        bool needsAuth = _ui->authRequiredcheckBox->isChecked();
        QString user = _ui->userLineEdit->text();
        QString pass = _ui->passwordLineEdit->text();
        cfgFile.setProxyType(type, _ui->hostLineEdit->text(),
            _ui->portSpinBox->value(), needsAuth, user, pass);
    }

    ClientProxy proxy;
    proxy.setupQtProxyFromConfig(); // Refresh the Qt proxy settings as the
    // quota check can happen all the time.

    // ...and set the folders dirty, they refresh their proxy next time they
    // start the sync.
    FolderMan::instance()->setDirtyProxy();

    for (auto account : AccountManager::instance()->accounts()) {
        account->freshConnectionAttempt();
    }
}

void NetworkSettings::saveBWLimitSettings()
{
    ConfigFile cfgFile;
    if (_ui->downloadLimitRadioButton->isChecked()) {
        cfgFile.setUseDownloadLimit(1);
    } else if (_ui->noDownloadLimitRadioButton->isChecked()) {
        cfgFile.setUseDownloadLimit(0);
    } else if (_ui->autoDownloadLimitRadioButton->isChecked()) {
        cfgFile.setUseDownloadLimit(-1);
    }
    cfgFile.setDownloadLimit(_ui->downloadSpinBox->value());

    if (_ui->uploadLimitRadioButton->isChecked()) {
        cfgFile.setUseUploadLimit(1);
    } else if (_ui->noUploadLimitRadioButton->isChecked()) {
        cfgFile.setUseUploadLimit(0);
    } else if (_ui->autoUploadLimitRadioButton->isChecked()) {
        cfgFile.setUseUploadLimit(-1);
    }
    cfgFile.setUploadLimit(_ui->uploadSpinBox->value());

    FolderMan::instance()->setDirtyNetworkLimits();
}

void NetworkSettings::checkEmptyProxyHost()
{
    if (_ui->hostLineEdit->isEnabled() && _ui->hostLineEdit->text().isEmpty()) {
        _ui->hostLineEdit->setStyleSheet("border: 1px solid red");
    } else {
        _ui->hostLineEdit->setStyleSheet(QString());
    }
}

void NetworkSettings::showEvent(QShowEvent *event)
{
    if (!event->spontaneous()
        && _ui->manualProxyRadioButton->isChecked()
        && _ui->hostLineEdit->text().isEmpty()) {
        _ui->noProxyRadioButton->setChecked(true);
        checkEmptyProxyHost();
        saveProxySettings();
    }

    QWidget::showEvent(event);
}

} // namespace OCC
