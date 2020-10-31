/*
 * Copyright (C) by Roeland Jago Douma <roeland@famdouma.nl>
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

#include "ui_sharedialog.h"
#include "sharedialog.h"
#include "sharee.h"
#include "sharelinkwidget.h"
#include "shareusergroupwidget.h"

#include "sharemanager.h"

#include "account.h"
#include "accountstate.h"
#include "configfile.h"
#include "theme.h"
#include "thumbnailjob.h"
#include "wordlist.h"

#include <QFileInfo>
#include <QFileIconProvider>
#include <QInputDialog>
#include <QPointer>
#include <QPushButton>
#include <QFrame>

namespace {
QString createRandomPassword()
{
    const auto words = OCC::WordList::getRandomWords(10);

    const auto addFirstLetter = [](const QString &current, const QString &next) {
        return current + next.at(0);
    };

    return std::accumulate(std::cbegin(words), std::cend(words), QString(), addFirstLetter);
}
}


namespace OCC {

static const int thumbnailSize = 40;

ShareDialog::ShareDialog(QPointer<AccountState> accountState,
    const QString &sharePath,
    const QString &localPath,
    SharePermissions maxSharingPermissions,
    const QByteArray &numericFileId,
    ShareDialogStartPage startPage,
    QWidget *parent)
    : QDialog(parent)
    , _ui(new Ui::ShareDialog)
    , _accountState(accountState)
    , _sharePath(sharePath)
    , _localPath(localPath)
    , _maxSharingPermissions(maxSharingPermissions)
    , _privateLinkUrl(accountState->account()->deprecatedPrivateLinkUrl(numericFileId).toString(QUrl::FullyEncoded))
    , _startPage(startPage)
{
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("SharingDialog"); // required as group for saveGeometry call

    _ui->setupUi(this);

    // We want to act on account state changes
    connect(_accountState.data(), &AccountState::stateChanged, this, &ShareDialog::slotAccountStateChanged);

    // Dialog visibility
    connect(this, &ShareDialog::onSetVisible, ownCloudGui::instance(), &ownCloudGui::slotDialogVisibilityChanged);

    // Set icon
    QFileInfo f_info(_localPath);
    QFileIconProvider icon_provider;
    QIcon icon = icon_provider.icon(f_info);
    auto pixmap = icon.pixmap(thumbnailSize, thumbnailSize);
    if (pixmap.width() > 0) {
        _ui->label_icon->setPixmap(pixmap);
    }

    // Set filename
    QFileInfo lPath(_localPath);
    QString fileName = lPath.fileName();
    _ui->label_name->setText(tr("%1").arg(fileName));
    QFont f(_ui->label_name->font());
    f.setPointSize(qRound(f.pointSize() * 1.4));
    _ui->label_name->setFont(f);

    QString ocDir(_sharePath);
    ocDir.truncate(ocDir.length() - fileName.length());

    ocDir.replace(QRegExp("^/*"), "");
    ocDir.replace(QRegExp("/*$"), "");

    // Laying this out is complex because sharePath
    // may be in use or not.
    _ui->gridLayout->removeWidget(_ui->label_sharePath);
    _ui->gridLayout->removeWidget(_ui->label_name);
    if (ocDir.isEmpty()) {
        _ui->gridLayout->addWidget(_ui->label_name, 0, 1, 2, 1);
        _ui->label_sharePath->setText(QString());
    } else {
        _ui->gridLayout->addWidget(_ui->label_name, 0, 1, 1, 1);
        _ui->gridLayout->addWidget(_ui->label_sharePath, 1, 1, 1, 1);
        _ui->label_sharePath->setText(tr("Folder: %2").arg(ocDir));
    }

    this->setWindowTitle(tr("%1 Sharing").arg(Theme::instance()->appNameGUI()));

    if (!accountState->account()->capabilities().shareAPI()) {
        return;
    }

    if (QFileInfo(_localPath).isFile()) {
        auto *job = new ThumbnailJob(_sharePath, _accountState->account(), this);
        connect(job, &ThumbnailJob::jobFinished, this, &ShareDialog::slotThumbnailFetched);
        job->start();
    }

    auto job = new PropfindJob(accountState->account(), _sharePath);
    job->setProperties(
        QList<QByteArray>()
        << "http://open-collaboration-services.org/ns:share-permissions"
        << "http://owncloud.org/ns:fileid" // numeric file id for fallback private link generation
        << "http://owncloud.org/ns:privatelink");
    job->setTimeout(10 * 1000);
    connect(job, &PropfindJob::result, this, &ShareDialog::slotPropfindReceived);
    connect(job, &PropfindJob::finishedWithError, this, &ShareDialog::slotPropfindError);
    job->start();

    bool sharingPossible = true;
    if (!accountState->account()->capabilities().sharePublicLink()) {
        qCWarning(lcSharing) << "Link shares have been disabled";
        sharingPossible = false;
    } else if (!(maxSharingPermissions & SharePermissionShare)) {
        qCWarning(lcSharing) << "The file can not be shared because it was shared without sharing permission.";
        sharingPossible = false;
    }

    if (sharingPossible) {
        _manager = new ShareManager(accountState->account(), this);
        connect(_manager, &ShareManager::sharesFetched, this, &ShareDialog::slotSharesFetched);
        connect(_manager, &ShareManager::linkShareCreated, this, &ShareDialog::slotAddLinkShareWidget);
        connect(_manager, &ShareManager::linkShareRequiresPassword, this, &ShareDialog::slotLinkShareRequiresPassword);
    }
}

void ShareDialog::addLinkShareWidget(const QSharedPointer<LinkShare> &linkShare){
    _linkWidgetList.append(new ShareLinkWidget(_accountState->account(), _sharePath, _localPath, _maxSharingPermissions, this));
    int index = _linkWidgetList.size()-1;
    _linkWidgetList.at(index)->setLinkShare(linkShare);

    connect(linkShare.data(), &Share::serverError, _linkWidgetList.at(index), &ShareLinkWidget::slotServerError);
    connect(linkShare.data(), &Share::shareDeleted, _linkWidgetList.at(index), &ShareLinkWidget::slotDeleteShareFetched);

    if(_manager) {
        connect(_manager, &ShareManager::linkShareRequiresPassword, _linkWidgetList.at(index), &ShareLinkWidget::slotCreateShareRequiresPassword);
        connect(_manager, &ShareManager::serverError, _linkWidgetList.at(index), &ShareLinkWidget::slotServerError);
    }

    // Connect all shares signals to gui slots
    connect(this, &ShareDialog::toggleAnimation, _linkWidgetList.at(index), &ShareLinkWidget::slotToggleAnimation);
    connect(_linkWidgetList.at(index), &ShareLinkWidget::createLinkShare, this, &ShareDialog::slotCreateLinkShare);
    connect(_linkWidgetList.at(index), &ShareLinkWidget::deleteLinkShare, this, &ShareDialog::slotDeleteShare);
    //connect(_linkWidgetList.at(index), &ShareLinkWidget::resizeRequested, this, &ShareDialog::slotAdjustScrollWidgetSize);

    // Connect styleChanged events to our widget, so it can adapt (Dark-/Light-Mode switching)
    connect(this, &ShareDialog::styleChanged, _linkWidgetList.at(index), &ShareLinkWidget::slotStyleChanged);

    _ui->verticalLayout->insertWidget(_linkWidgetList.size()+1, _linkWidgetList.at(index));
    _linkWidgetList.at(index)->setupUiOptions();
}

void ShareDialog::initLinkShareWidget(){
    if(_linkWidgetList.size() == 0){
        _emptyShareLinkWidget = new ShareLinkWidget(_accountState->account(), _sharePath, _localPath, _maxSharingPermissions, this);
        _linkWidgetList.append(_emptyShareLinkWidget);

        if (_manager) {
            connect(_manager, &ShareManager::linkShareRequiresPassword, _emptyShareLinkWidget, &ShareLinkWidget::slotCreateShareRequiresPassword);
        }

        connect(_emptyShareLinkWidget, &ShareLinkWidget::resizeRequested, this, &ShareDialog::slotAdjustScrollWidgetSize);
//        connect(this, &ShareDialog::toggleAnimation, _emptyShareLinkWidget, &ShareLinkWidget::slotToggleAnimation);
        connect(_emptyShareLinkWidget, &ShareLinkWidget::createLinkShare, this, &ShareDialog::slotCreateLinkShare);

        _ui->verticalLayout->insertWidget(_linkWidgetList.size()+1, _emptyShareLinkWidget);
        _emptyShareLinkWidget->show();

    } else if(_emptyShareLinkWidget) {
        _emptyShareLinkWidget->hide();
        _ui->verticalLayout->removeWidget(_emptyShareLinkWidget);
        _linkWidgetList.removeAll(_emptyShareLinkWidget);
        _emptyShareLinkWidget = nullptr;
    }
}

void ShareDialog::slotAddLinkShareWidget(const QSharedPointer<LinkShare> &linkShare){
    emit toggleAnimation(true);
    addLinkShareWidget(linkShare);
    initLinkShareWidget();
    emit toggleAnimation(false);
}

void ShareDialog::slotSharesFetched(const QList<QSharedPointer<Share>> &shares)
{
    emit toggleAnimation(true);

    const QString versionString = _accountState->account()->serverVersion();
    qCInfo(lcSharing) << versionString << "Fetched" << shares.count() << "shares";
    foreach (auto share, shares) {
        if (share->getShareType() != Share::TypeLink || share->getUidOwner() != share->account()->davUser()) {
            continue;
        }

        QSharedPointer<LinkShare> linkShare = qSharedPointerDynamicCast<LinkShare>(share);
        addLinkShareWidget(linkShare);
    }

    initLinkShareWidget();
    emit toggleAnimation(false);
}

void ShareDialog::slotAdjustScrollWidgetSize()
{
    int count = this->findChildren<ShareLinkWidget *>().count();
    _ui->scrollArea->setVisible(count > 0);
    if (count > 0 && count <= 3) {
        _ui->scrollArea->setFixedHeight(_ui->scrollArea->widget()->sizeHint().height());
    }
    _ui->scrollArea->setFrameShape(count > 3 ? QFrame::StyledPanel : QFrame::NoFrame);
}

ShareDialog::~ShareDialog()
{
    _linkWidgetList.clear();
    delete _ui;
}

void ShareDialog::done(int r)
{
    ConfigFile cfg;
    cfg.saveGeometry(this);
    QDialog::done(r);
}

void ShareDialog::slotPropfindReceived(const QVariantMap &result)
{
    const QVariant receivedPermissions = result["share-permissions"];
    if (!receivedPermissions.toString().isEmpty()) {
        _maxSharingPermissions = static_cast<SharePermissions>(receivedPermissions.toInt());
        qCInfo(lcSharing) << "Received sharing permissions for" << _sharePath << _maxSharingPermissions;
    }
    auto privateLinkUrl = result["privatelink"].toString();
    auto numericFileId = result["fileid"].toByteArray();
    if (!privateLinkUrl.isEmpty()) {
        qCInfo(lcSharing) << "Received private link url for" << _sharePath << privateLinkUrl;
        _privateLinkUrl = privateLinkUrl;
    } else if (!numericFileId.isEmpty()) {
        qCInfo(lcSharing) << "Received numeric file id for" << _sharePath << numericFileId;
        _privateLinkUrl = _accountState->account()->deprecatedPrivateLinkUrl(numericFileId).toString(QUrl::FullyEncoded);
    }

    showSharingUi();
}

void ShareDialog::slotPropfindError()
{
    // On error show the share ui anyway. The user can still see shares,
    // delete them and so on, even though adding new shares or granting
    // some of the permissions might fail.

    showSharingUi();
}

void ShareDialog::showSharingUi()
{
    auto theme = Theme::instance();

    // There's no difference between being unable to reshare and
    // being unable to reshare with reshare permission.
    bool canReshare = _maxSharingPermissions & SharePermissionShare;

    if (!canReshare) {
        auto label = new QLabel(this);
        label->setText(tr("The file can not be shared because it was shared without sharing permission."));
        label->setWordWrap(true);
        _ui->verticalLayout->insertWidget(1, label);
        return;
    }

    // We only do user/group sharing from 8.2.0
    bool userGroupSharing =
        theme->userGroupSharing()
        && _accountState->account()->serverVersionInt() >= Account::makeServerVersion(8, 2, 0);

    if (userGroupSharing) {
        _userGroupWidget = new ShareUserGroupWidget(_accountState->account(), _sharePath, _localPath, _maxSharingPermissions, _privateLinkUrl, this);

        // Connect styleChanged events to our widget, so it can adapt (Dark-/Light-Mode switching)
        connect(this, &ShareDialog::styleChanged, _userGroupWidget, &ShareUserGroupWidget::slotStyleChanged);

        _ui->verticalLayout->insertWidget(1, _userGroupWidget);
        _userGroupWidget->getShares();
    }

    if (theme->linkSharing()) {
        if(_manager) {
            _manager->fetchShares(_sharePath);
        }
    }
}

void ShareDialog::slotCreateLinkShare()
{
    if(_manager) {
        const auto askOptionalPassword = _accountState->account()->capabilities().sharePublicLinkAskOptionalPassword();
        const auto password = askOptionalPassword ? createRandomPassword() : QString();
        _manager->createLinkShare(_sharePath, QString(), password);
    }
}

void ShareDialog::slotLinkShareRequiresPassword()
{
    bool ok = false;
    QString password = QInputDialog::getText(this,
                                             tr("Password for share required"),
                                             tr("Please enter a password for your link share:"),
                                             QLineEdit::Normal,
                                             QString(),
                                             &ok);

    if (!ok) {
        // The dialog was canceled so no need to do anything
        return;
    }

    if(_manager) {
        // Try to create the link share again with the newly entered password
        _manager->createLinkShare(_sharePath, QString(), password);
    }
}

void ShareDialog::slotDeleteShare()
{
    auto sharelinkWidget = dynamic_cast<ShareLinkWidget*>(sender());
    sharelinkWidget->hide();
    _ui->verticalLayout->removeWidget(sharelinkWidget);
    _linkWidgetList.removeAll(sharelinkWidget);
    initLinkShareWidget();
}

void ShareDialog::slotThumbnailFetched(const int &statusCode, const QByteArray &reply)
{
    if (statusCode != 200) {
        qCWarning(lcSharing) << "Thumbnail status code: " << statusCode;
        return;
    }

    QPixmap p;
    p.loadFromData(reply, "PNG");
    p = p.scaledToHeight(thumbnailSize, Qt::SmoothTransformation);
    _ui->label_icon->setPixmap(p);
    _ui->label_icon->show();
}

void ShareDialog::slotAccountStateChanged(int state)
{
    bool enabled = (state == AccountState::State::Connected);
    qCDebug(lcSharing) << "Account connected?" << enabled;

    if (_userGroupWidget) {
        _userGroupWidget->setEnabled(enabled);
    }

    if(_linkWidgetList.size() > 0){
        foreach(ShareLinkWidget *widget, _linkWidgetList){
            widget->setEnabled(state);
        }
    }
}

void ShareDialog::changeEvent(QEvent *e)
{
    switch (e->type()) {
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
    case QEvent::ThemeChange:
        // Notify the other widgets (Dark-/Light-Mode switching)
        emit styleChanged();
        break;
    default:
        break;
    }

    QDialog::changeEvent(e);
}

void ShareDialog::setVisible(bool visible)
{
    emit onSetVisible(visible);
    QDialog::setVisible(visible);
}

} // namespace OCC
