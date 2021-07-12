/*
 * Copyright (C) by Roeland Jago Douma <roeland@owncloud.com>
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

#include "sharee.h"
#include "ui_shareusergroupwidget.h"
#include "ui_shareuserline.h"
#include "shareusergroupwidget.h"
#include "account.h"
#include "folderman.h"
#include "folder.h"
#include "accountmanager.h"
#include "theme.h"
#include "configfile.h"
#include "capabilities.h"
#include "guiutility.h"
#include "thumbnailjob.h"
#include "sharemanager.h"
#include "theme.h"

#include "QProgressIndicator.h"
#include <QBuffer>
#include <QFileIconProvider>
#include <QClipboard>
#include <QFileInfo>
#include <QAbstractProxyModel>
#include <QCompleter>
#include <qlayout.h>
#include <QPropertyAnimation>
#include <QMenu>
#include <QAction>
#include <QDesktopServices>
#include <QInputDialog>
#include <QMessageBox>
#include <QCryptographicHash>
#include <QColor>
#include <QPainter>
#include <QListWidget>

#include <cstring>

namespace {
    const char *passwordIsSetPlaceholder = "●●●●●●●●";
}

namespace OCC {

ShareUserGroupWidget::ShareUserGroupWidget(AccountPtr account,
    const QString &sharePath,
    const QString &localPath,
    SharePermissions maxSharingPermissions,
    const QString &privateLinkUrl,
    QWidget *parent)
    : QWidget(parent)
    , _ui(new Ui::ShareUserGroupWidget)
    , _account(account)
    , _sharePath(sharePath)
    , _localPath(localPath)
    , _maxSharingPermissions(maxSharingPermissions)
    , _privateLinkUrl(privateLinkUrl)
    , _disableCompleterActivated(false)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setObjectName("SharingDialogUG"); // required as group for saveGeometry call

    _ui->setupUi(this);

    //Is this a file or folder?
    _isFile = QFileInfo(localPath).isFile();

    _completer = new QCompleter(this);
    _completerModel = new ShareeModel(_account,
        _isFile ? QLatin1String("file") : QLatin1String("folder"),
        _completer);
    connect(_completerModel, &ShareeModel::shareesReady, this, &ShareUserGroupWidget::slotShareesReady);
    connect(_completerModel, &ShareeModel::displayErrorMessage, this, &ShareUserGroupWidget::displayError);

    _completer->setModel(_completerModel);
    _completer->setCaseSensitivity(Qt::CaseInsensitive);
    _completer->setCompletionMode(QCompleter::UnfilteredPopupCompletion);
    _ui->shareeLineEdit->setCompleter(_completer);

    auto searchGloballyAction = new QAction(_ui->shareeLineEdit);
    searchGloballyAction->setIcon(QIcon(":/client/theme/magnifying-glass.svg"));
    searchGloballyAction->setToolTip(tr("Search globally"));

    connect(searchGloballyAction, &QAction::triggered, this, [this]() {
        searchForSharees(ShareeModel::GlobalSearch);
    });

    _ui->shareeLineEdit->addAction(searchGloballyAction, QLineEdit::LeadingPosition);

    _manager = new ShareManager(_account, this);
    connect(_manager, &ShareManager::sharesFetched, this, &ShareUserGroupWidget::slotSharesFetched);
    connect(_manager, &ShareManager::shareCreated, this, &ShareUserGroupWidget::slotShareCreated);
    connect(_manager, &ShareManager::serverError, this, &ShareUserGroupWidget::displayError);
    connect(_ui->shareeLineEdit, &QLineEdit::returnPressed, this, &ShareUserGroupWidget::slotLineEditReturn);
    connect(_ui->confirmShare, &QAbstractButton::clicked, this, &ShareUserGroupWidget::slotLineEditReturn);
    //TODO connect(_ui->privateLinkText, &QLabel::linkActivated, this, &ShareUserGroupWidget::slotPrivateLinkShare);

    // By making the next two QueuedConnections we can override
    // the strings the completer sets on the line edit.
    connect(_completer, SIGNAL(activated(QModelIndex)), SLOT(slotCompleterActivated(QModelIndex)),
        Qt::QueuedConnection);
    connect(_completer, SIGNAL(highlighted(QModelIndex)), SLOT(slotCompleterHighlighted(QModelIndex)),
        Qt::QueuedConnection);

    // Queued connection so this signal is recieved after textChanged
    connect(_ui->shareeLineEdit, &QLineEdit::textEdited,
        this, &ShareUserGroupWidget::slotLineEditTextEdited, Qt::QueuedConnection);
    _ui->shareeLineEdit->installEventFilter(this);
    connect(&_completionTimer, &QTimer::timeout, this, [this]() {
        searchForSharees(ShareeModel::LocalSearch);
    });
    _completionTimer.setSingleShot(true);
    _completionTimer.setInterval(600);

    _ui->errorLabel->hide();

    // TODO Progress Indicator where should it go?
    // Setup the sharee search progress indicator
    //_ui->shareeHorizontalLayout->addWidget(&_pi_sharee);

    _parentScrollArea = parentWidget()->findChild<QScrollArea*>("scrollArea");

    customizeStyle();
}

ShareUserGroupWidget::~ShareUserGroupWidget()
{
    delete _ui;
}

void ShareUserGroupWidget::on_shareeLineEdit_textChanged(const QString &)
{
    _completionTimer.stop();
    emit togglePublicLinkShare(false);
}

void ShareUserGroupWidget::slotLineEditTextEdited(const QString &text)
{
    _disableCompleterActivated = false;
    // First textChanged is called first and we stopped the timer when the text is changed, programatically or not
    // Then we restart the timer here if the user touched a key
    if (!text.isEmpty()) {
        _completionTimer.start();
        emit togglePublicLinkShare(true);
    }
}

void ShareUserGroupWidget::slotLineEditReturn()
{
    _disableCompleterActivated = false;
    // did the user type in one of the options?
    const auto text = _ui->shareeLineEdit->text();
    for (int i = 0; i < _completerModel->rowCount(); ++i) {
        const auto sharee = _completerModel->getSharee(i);
        if (sharee->format() == text
            || sharee->displayName() == text
            || sharee->shareWith() == text) {
            slotCompleterActivated(_completerModel->index(i));
            // make sure we do not send the same item twice (because return is called when we press
            // return to activate an item inthe completer)
            _disableCompleterActivated = true;
            return;
        }
    }

    // nothing found? try to refresh completion
    _completionTimer.start();
}

void ShareUserGroupWidget::searchForSharees(ShareeModel::LookupMode lookupMode)
{
    if (_ui->shareeLineEdit->text().isEmpty()) {
        return;
    }

    _ui->shareeLineEdit->setEnabled(false);
    _completionTimer.stop();
    _pi_sharee.startAnimation();
    ShareeModel::ShareeSet blacklist;

    // Add the current user to _sharees since we can't share with ourself
    QSharedPointer<Sharee> currentUser(new Sharee(_account->credentials()->user(), "", Sharee::Type::User));
    blacklist << currentUser;

    foreach (auto sw, _parentScrollArea->findChildren<ShareUserLine *>()) {
        blacklist << sw->share()->getShareWith();
    }
    _ui->errorLabel->hide();
    _completerModel->fetch(_ui->shareeLineEdit->text(), blacklist, lookupMode);
}

void ShareUserGroupWidget::getShares()
{
    _manager->fetchShares(_sharePath);
}

void ShareUserGroupWidget::slotShareCreated(const QSharedPointer<Share> &share)
{
    if (share && _account->capabilities().shareEmailPasswordEnabled() && !_account->capabilities().shareEmailPasswordEnforced()) {
        // remember this share Id so we can set it's password Line Edit to focus later
        _lastCreatedShareId = share->getId();
    }
    // fetch all shares including the one we've just created
    getShares();
}

void ShareUserGroupWidget::slotSharesFetched(const QList<QSharedPointer<Share>> &shares)
{
    QScrollArea *scrollArea = _parentScrollArea;

    auto newViewPort = new QWidget(scrollArea);
    auto layout = new QVBoxLayout(newViewPort);
    layout->setContentsMargins(0, 0, 0, 0);
    int x = 0;
    int height = 0;
    QList<QString> linkOwners({});

    ShareUserLine *justCreatedShareThatNeedsPassword = nullptr;

    foreach (const auto &share, shares) {
        // We don't handle link shares, only TypeUser or TypeGroup
        if (share->getShareType() == Share::TypeLink) {
            if(!share->getUidOwner().isEmpty() &&
                    share->getUidOwner() != share->account()->davUser()){
                linkOwners.append(share->getOwnerDisplayName());
             }
            continue;
        }

        // the owner of the file that shared it first
        // leave out if it's the current user
        if(x == 0 && !share->getUidOwner().isEmpty() && !(share->getUidOwner() == _account->credentials()->user())) {
            _ui->mainOwnerLabel->setText(QString("Shared with you by ").append(share->getOwnerDisplayName()));
        }


        Q_ASSERT(share->getShareType() == Share::TypeUser || share->getShareType() == Share::TypeGroup || share->getShareType() == Share::TypeEmail);
        auto userGroupShare = qSharedPointerDynamicCast<UserGroupShare>(share);
        auto *s = new ShareUserLine(_account, userGroupShare, _maxSharingPermissions, _isFile, _parentScrollArea);
        connect(s, &ShareUserLine::resizeRequested, this, &ShareUserGroupWidget::slotAdjustScrollWidgetSize);
        connect(s, &ShareUserLine::visualDeletionDone, this, &ShareUserGroupWidget::getShares);
        s->setBackgroundRole(layout->count() % 2 == 0 ? QPalette::Base : QPalette::AlternateBase);

        // Connect styleChanged events to our widget, so it can adapt (Dark-/Light-Mode switching)
        connect(this, &ShareUserGroupWidget::styleChanged, s, &ShareUserLine::slotStyleChanged);

        layout->addWidget(s);

        if (!_lastCreatedShareId.isEmpty() && share->getId() == _lastCreatedShareId) {
            _lastCreatedShareId = QString();
            if (_account->capabilities().shareEmailPasswordEnabled() && !_account->capabilities().shareEmailPasswordEnforced()) {
                justCreatedShareThatNeedsPassword = s;
            }
        }

        x++;
        if (x <= 3) {
            height = newViewPort->sizeHint().height();
        }
    }

    foreach (const QString &owner, linkOwners) {
        auto ownerLabel = new QLabel(QString(owner + " shared via link"));
        layout->addWidget(ownerLabel);
        ownerLabel->setVisible(true);

        x++;
        if (x <= 6) {
            height = newViewPort->sizeHint().height();
        }
    }

    scrollArea->setFrameShape(x > 6 ? QFrame::StyledPanel : QFrame::NoFrame);
    scrollArea->setVisible(!shares.isEmpty());
    scrollArea->setFixedHeight(height);
    scrollArea->setWidget(newViewPort);

    _disableCompleterActivated = false;
    activateShareeLineEdit();

    if (justCreatedShareThatNeedsPassword) {
        // always set focus to a password Line Edit when the new email share is created on a server with optional passwords enabled for email shares
        justCreatedShareThatNeedsPassword->focusPasswordLineEdit();
    }
}

void ShareUserGroupWidget::slotAdjustScrollWidgetSize()
{
    QScrollArea *scrollArea = _parentScrollArea;
    const auto shareUserLineChilds = scrollArea->findChildren<ShareUserLine *>();

    // Ask the child widgets to calculate their size
    for (const auto shareUserLineChild : shareUserLineChilds) {
        shareUserLineChild->adjustSize();
    }

    const auto shareUserLineChildsCount = shareUserLineChilds.count();
    scrollArea->setVisible(shareUserLineChildsCount > 0);
    if (shareUserLineChildsCount > 0 && shareUserLineChildsCount <= 3) {
        scrollArea->setFixedHeight(scrollArea->widget()->sizeHint().height());
    }
    scrollArea->setFrameShape(shareUserLineChildsCount > 3 ? QFrame::StyledPanel : QFrame::NoFrame);
}

void ShareUserGroupWidget::slotPrivateLinkShare()
{
    auto menu = new QMenu(this);
    menu->setAttribute(Qt::WA_DeleteOnClose);

    // this icon is not handled by slotStyleChanged() -> customizeStyle but we can live with that
    menu->addAction(Theme::createColorAwareIcon(":/client/theme/copy.svg"),
                    tr("Copy link"),
        this, SLOT(slotPrivateLinkCopy()));

    menu->exec(QCursor::pos());
}

void ShareUserGroupWidget::slotShareesReady()
{
    activateShareeLineEdit();

    _pi_sharee.stopAnimation();
    if (_completerModel->rowCount() == 0) {
        displayError(0, tr("No results for \"%1\"").arg(_completerModel->currentSearch()));
    }

    // if no rows are present in the model - complete() will hide the completer
    _completer->complete();
}

void ShareUserGroupWidget::slotCompleterActivated(const QModelIndex &index)
{
    if (_disableCompleterActivated)
        return;
    // The index is an index from the QCompletion model which is itelf a proxy
    // model proxying the _completerModel
    auto sharee = qvariant_cast<QSharedPointer<Sharee>>(index.data(Qt::UserRole));
    if (sharee.isNull()) {
        return;
    }

// TODO Progress Indicator where should it go?
//    auto indicator = new QProgressIndicator(viewPort);
//    indicator->startAnimation();
//    if (layout->count() == 1) {
//        // No shares yet! Remove the label, add some stretch.
//        delete layout->itemAt(0)->widget();
//        layout->addStretch(1);
//    }
//    layout->insertWidget(layout->count() - 1, indicator);

    /*
     * Don't send the reshare permissions for federated shares for servers <9.1
     * https://github.com/owncloud/core/issues/22122#issuecomment-185637344
     * https://github.com/owncloud/client/issues/4996
     */

    _lastCreatedShareId = QString();

    if (sharee->type() == Sharee::Federated
        && _account->serverVersionInt() < Account::makeServerVersion(9, 1, 0)) {
        int permissions = SharePermissionRead | SharePermissionUpdate;
        if (!_isFile) {
            permissions |= SharePermissionCreate | SharePermissionDelete;
        }
        _manager->createShare(_sharePath, Share::ShareType(sharee->type()),
            sharee->shareWith(), SharePermission(permissions));
    } else {
        QString password;
        if (sharee->type() == Sharee::Email && _account->capabilities().shareEmailPasswordEnforced()) {
            _ui->shareeLineEdit->clear();
            // always show a dialog for password-enforced email shares
            bool ok = false;

            do {
                password = QInputDialog::getText(
                            this,
                            tr("Password for share required"),
                            tr("Please enter a password for your email share:"),
                            QLineEdit::Password,
                            QString(),
                            &ok);
            } while (password.isEmpty() && ok);

            if (!ok) {
                return;
            }
        }

        // Default permissions on creation
        int permissions = SharePermissionCreate | SharePermissionUpdate
                | SharePermissionDelete | SharePermissionShare;
        _manager->createShare(_sharePath, Share::ShareType(sharee->type()),
            sharee->shareWith(), SharePermission(permissions), password);
    }

    _ui->shareeLineEdit->setEnabled(false);
    _ui->shareeLineEdit->clear();
}

void ShareUserGroupWidget::slotCompleterHighlighted(const QModelIndex &index)
{
    // By default the completer would set the text to EditRole,
    // override that here.
    _ui->shareeLineEdit->setText(index.data(Qt::DisplayRole).toString());
}

void ShareUserGroupWidget::displayError(int code, const QString &message)
{
    _pi_sharee.stopAnimation();

    // Also remove the spinner in the widget list, if any
    foreach (auto pi, _parentScrollArea->findChildren<QProgressIndicator *>()) {
        delete pi;
    }

    qCWarning(lcSharing) << "Sharing error from server" << code << message;
    _ui->errorLabel->setText(message);
    _ui->errorLabel->show();
    activateShareeLineEdit();
}

void ShareUserGroupWidget::slotPrivateLinkOpenBrowser()
{
    Utility::openBrowser(_privateLinkUrl, this);
}

void ShareUserGroupWidget::slotPrivateLinkCopy()
{
    QApplication::clipboard()->setText(_privateLinkUrl);
}

void ShareUserGroupWidget::slotPrivateLinkEmail()
{
    Utility::openEmailComposer(
        tr("I shared something with you"),
        _privateLinkUrl,
        this);
}

void ShareUserGroupWidget::slotStyleChanged()
{
    customizeStyle();

    // Notify the other widgets (ShareUserLine in this case, Dark-/Light-Mode switching)
    emit styleChanged();
}

void ShareUserGroupWidget::customizeStyle()
{
    _ui->confirmShare->setIcon(Theme::createColorAwareIcon(":/client/theme/confirm.svg"));

    _pi_sharee.setColor(QGuiApplication::palette().color(QPalette::Text));

    foreach (auto pi, _parentScrollArea->findChildren<QProgressIndicator *>()) {
        pi->setColor(QGuiApplication::palette().color(QPalette::Text));;
    }
}

void ShareUserGroupWidget::activateShareeLineEdit()
{
    _ui->shareeLineEdit->setEnabled(true);
    _ui->shareeLineEdit->setFocus();
}

ShareUserLine::ShareUserLine(AccountPtr account,
                             QSharedPointer<UserGroupShare> share,
                             SharePermissions maxSharingPermissions,
                             bool isFile,
                             QWidget *parent)
    : QWidget(parent)
    , _ui(new Ui::ShareUserLine)
    , _account(account)
    , _share(share)
    , _isFile(isFile)
{
    Q_ASSERT(_share);
    _ui->setupUi(this);

    _ui->sharedWith->setElideMode(Qt::ElideRight);
    _ui->sharedWith->setText(share->getShareWith()->format());

    // adds permissions
    // can edit permission
    bool enabled = (maxSharingPermissions & SharePermissionUpdate);
    if(!_isFile) enabled = enabled && (maxSharingPermissions & SharePermissionCreate &&
                                      maxSharingPermissions & SharePermissionDelete);
    _ui->permissionsEdit->setEnabled(enabled);
    connect(_ui->permissionsEdit, &QAbstractButton::clicked, this, &ShareUserLine::slotEditPermissionsChanged);
    connect(_ui->noteConfirmButton, &QAbstractButton::clicked, this, &ShareUserLine::onNoteConfirmButtonClicked);
    connect(_ui->confirmExpirationDate, &QAbstractButton::clicked, this, &ShareUserLine::setExpireDate);
    connect(_ui->calendar, &QDateTimeEdit::dateChanged, this, &ShareUserLine::setExpireDate);

    connect(_share.data(), &UserGroupShare::noteSet, this, &ShareUserLine::disableProgessIndicatorAnimation);
    connect(_share.data(), &UserGroupShare::noteSetError, this, &ShareUserLine::disableProgessIndicatorAnimation);
    connect(_share.data(), &UserGroupShare::expireDateSet, this, &ShareUserLine::disableProgessIndicatorAnimation);

    connect(_ui->confirmPassword, &QToolButton::clicked, this, &ShareUserLine::slotConfirmPasswordClicked);
    connect(_ui->lineEdit_password, &QLineEdit::returnPressed, this, &ShareUserLine::slotLineEditPasswordReturnPressed);

    // create menu with checkable permissions
    auto *menu = new QMenu(this);
    _permissionReshare= new QAction(tr("Can reshare"), this);
    _permissionReshare->setCheckable(true);
    _permissionReshare->setEnabled(maxSharingPermissions & SharePermissionShare);
    menu->addAction(_permissionReshare);
    connect(_permissionReshare, &QAction::triggered, this, &ShareUserLine::slotPermissionsChanged);

    showNoteOptions(false);

    // email shares do not support notes and expiration dates
    const bool isNoteAndExpirationDateSupported = _share->getShareType() != Share::ShareType::TypeEmail;

    if (isNoteAndExpirationDateSupported) {
        _noteLinkAction = new QAction(tr("Note to recipient"));
        _noteLinkAction->setCheckable(true);
        menu->addAction(_noteLinkAction);
        connect(_noteLinkAction, &QAction::triggered, this, &ShareUserLine::toggleNoteOptions);
        if (!_share->getNote().isEmpty()) {
            _noteLinkAction->setChecked(true);
            showNoteOptions(true);
        }
    }

    showExpireDateOptions(false);

    if (isNoteAndExpirationDateSupported) {
        // email shares do not support expiration dates
        _expirationDateLinkAction = new QAction(tr("Set expiration date"));
        _expirationDateLinkAction->setCheckable(true);
        menu->addAction(_expirationDateLinkAction);
        connect(_expirationDateLinkAction, &QAction::triggered, this, &ShareUserLine::toggleExpireDateOptions);
        const auto expireDate = _share->getExpireDate().isValid() ? share.data()->getExpireDate() : QDate();
        if (!expireDate.isNull()) {
            _ui->calendar->setDate(expireDate);
            _expirationDateLinkAction->setChecked(true);
            showExpireDateOptions(true);
        }
    }

    menu->addSeparator();

      // Adds action to delete share widget
      QIcon deleteicon = QIcon::fromTheme(QLatin1String("user-trash"),QIcon(QLatin1String(":/client/theme/delete.svg")));
      _deleteShareButton= new QAction(deleteicon,tr("Unshare"), this);

    menu->addAction(_deleteShareButton);
    connect(_deleteShareButton, &QAction::triggered, this, &ShareUserLine::on_deleteShareButton_clicked);

    /*
     * Files can't have create or delete permissions
     */
    if (!_isFile) {
        _permissionCreate = new QAction(tr("Can create"), this);
        _permissionCreate->setCheckable(true);
        _permissionCreate->setEnabled(maxSharingPermissions & SharePermissionCreate);
        menu->addAction(_permissionCreate);
        connect(_permissionCreate, &QAction::triggered, this, &ShareUserLine::slotPermissionsChanged);

        _permissionChange = new QAction(tr("Can change"), this);
        _permissionChange->setCheckable(true);
        _permissionChange->setEnabled(maxSharingPermissions & SharePermissionUpdate);
        menu->addAction(_permissionChange);
        connect(_permissionChange, &QAction::triggered, this, &ShareUserLine::slotPermissionsChanged);

        _permissionDelete = new QAction(tr("Can delete"), this);
        _permissionDelete->setCheckable(true);
        _permissionDelete->setEnabled(maxSharingPermissions & SharePermissionDelete);
        menu->addAction(_permissionDelete);
        connect(_permissionDelete, &QAction::triggered, this, &ShareUserLine::slotPermissionsChanged);
    }

    // Adds action to display password widget (check box)
    if (_share->getShareType() == Share::TypeEmail && (_share->isPasswordSet() || _account->capabilities().shareEmailPasswordEnabled())) {
        _passwordProtectLinkAction = new QAction(tr("Password protect"), this);
        _passwordProtectLinkAction->setCheckable(true);
        _passwordProtectLinkAction->setChecked(_share->isPasswordSet());
        // checkbox can be checked/unchedkec if the password is not yet set or if it's not enforced
        _passwordProtectLinkAction->setEnabled(!_share->isPasswordSet() || !_account->capabilities().shareEmailPasswordEnforced());

        menu->addAction(_passwordProtectLinkAction);
        connect(_passwordProtectLinkAction, &QAction::triggered, this, &ShareUserLine::slotPasswordCheckboxChanged);

        refreshPasswordLineEditPlaceholder();

        connect(_share.data(), &Share::passwordSet, this, &ShareUserLine::slotPasswordSet);
        connect(_share.data(), &Share::passwordSetError, this, &ShareUserLine::slotPasswordSetError);
    }

    refreshPasswordOptions();

    _ui->errorLabel->hide();

    _ui->permissionToolButton->setMenu(menu);
    _ui->permissionToolButton->setPopupMode(QToolButton::InstantPopup);

    _ui->passwordProgressIndicator->setVisible(false);

    // Set the permissions checkboxes
    displayPermissions();

    /*
     * We don't show permission share for federated shares with server <9.1
     * https://github.com/owncloud/core/issues/22122#issuecomment-185637344
     * https://github.com/owncloud/client/issues/4996
     */
    if (share->getShareType() == Share::TypeRemote
        && share->account()->serverVersionInt() < Account::makeServerVersion(9, 1, 0)) {
        _permissionReshare->setVisible(false);
        _ui->permissionToolButton->setVisible(false);
    }

    connect(share.data(), &Share::permissionsSet, this, &ShareUserLine::slotPermissionsSet);
    connect(share.data(), &Share::shareDeleted, this, &ShareUserLine::slotShareDeleted);

    if (!share->account()->capabilities().shareResharing()) {
        _permissionReshare->setVisible(false);
    }

    loadAvatar();

    customizeStyle();
}

void ShareUserLine::loadAvatar()
{
    const int avatarSize = 36;

    // Set size of the placeholder
    _ui->avatar->setMinimumHeight(avatarSize);
    _ui->avatar->setMinimumWidth(avatarSize);
    _ui->avatar->setMaximumHeight(avatarSize);
    _ui->avatar->setMaximumWidth(avatarSize);
    _ui->avatar->setAlignment(Qt::AlignCenter);

    /* Create the fallback avatar.
     *
     * This will be shown until the avatar image data arrives.
     */
    const QByteArray hash = QCryptographicHash::hash(_ui->sharedWith->text().toUtf8(), QCryptographicHash::Md5);
    double hue = static_cast<quint8>(hash[0]) / 255.;

    // See core/js/placeholder.js for details on colors and styling
    const QColor bg = QColor::fromHslF(hue, 0.7, 0.68);
    const QString style = QString(R"(* {
        color: #fff;
        background-color: %1;
        border-radius: %2px;
        text-align: center;
        line-height: %2px;
        font-size: %2px;
    })").arg(bg.name(), QString::number(avatarSize / 2));
    _ui->avatar->setStyleSheet(style);

    // The avatar label is the first character of the user name.
    const QString text = _share->getShareWith()->displayName();
    _ui->avatar->setText(text.at(0).toUpper());

    /* Start the network job to fetch the avatar data.
     *
     * Currently only regular users can have avatars.
     */
    if (_share->getShareWith()->type() == Sharee::User) {
        auto *job = new AvatarJob(_share->account(), _share->getShareWith()->shareWith(), avatarSize, this);
        connect(job, &AvatarJob::avatarPixmap, this, &ShareUserLine::slotAvatarLoaded);
        job->start();
    }
}

void ShareUserLine::slotAvatarLoaded(QImage avatar)
{
    if (avatar.isNull())
        return;

    avatar = AvatarJob::makeCircularAvatar(avatar);
    _ui->avatar->setPixmap(QPixmap::fromImage(avatar));

    // Remove the stylesheet for the fallback avatar
    _ui->avatar->setStyleSheet("");
}

void ShareUserLine::on_deleteShareButton_clicked()
{
    setEnabled(false);
    _share->deleteShare();
}

ShareUserLine::~ShareUserLine()
{
    delete _ui;
}

void ShareUserLine::slotEditPermissionsChanged()
{
    setEnabled(false);

    // Can never manually be set to "partial".
    // This works because the state cycle for clicking is
    // unchecked -> partial -> checked -> unchecked.
    if (_ui->permissionsEdit->checkState() == Qt::PartiallyChecked) {
        _ui->permissionsEdit->setCheckState(Qt::Checked);
    }

    Share::Permissions permissions = SharePermissionRead;

    //  folders edit = CREATE, READ, UPDATE, DELETE
    //  files edit = READ + UPDATE
    if (_ui->permissionsEdit->checkState() == Qt::Checked) {

        /*
         * Files can't have create or delete permisisons
         */
        if (!_isFile) {
            if (_permissionChange->isEnabled())
                permissions |= SharePermissionUpdate;
            if (_permissionCreate->isEnabled())
                permissions |= SharePermissionCreate;
            if (_permissionDelete->isEnabled())
                permissions |= SharePermissionDelete;
        } else {
            permissions |= SharePermissionUpdate;
        }
    }

    if(_isFile && _permissionReshare->isEnabled() && _permissionReshare->isChecked())
        permissions |= SharePermissionShare;

    _share->setPermissions(permissions);
}

void ShareUserLine::slotPermissionsChanged()
{
    setEnabled(false);

    Share::Permissions permissions = SharePermissionRead;

    if (_permissionReshare->isChecked())
        permissions |= SharePermissionShare;

    if (!_isFile) {
        if (_permissionChange->isChecked())
            permissions |= SharePermissionUpdate;
        if (_permissionCreate->isChecked())
            permissions |= SharePermissionCreate;
        if (_permissionDelete->isChecked())
            permissions |= SharePermissionDelete;
    } else {
        if (_ui->permissionsEdit->isChecked())
            permissions |= SharePermissionUpdate;
    }

    _share->setPermissions(permissions);
}

void ShareUserLine::slotPasswordCheckboxChanged()
{
    if (!_passwordProtectLinkAction->isChecked()) {
        _ui->errorLabel->hide();
        _ui->errorLabel->clear();

        if (!_share->isPasswordSet()) {
            _ui->lineEdit_password->clear();
            refreshPasswordOptions();
        } else {
            // do not call refreshPasswordOptions here, as it will be called after the network request is complete
            togglePasswordSetProgressAnimation(true);
            _share->setPassword(QString());
        }
    } else {
        refreshPasswordOptions();

        if (_ui->lineEdit_password->isVisible() && _ui->lineEdit_password->isEnabled()) {
            focusPasswordLineEdit();
        }
    }
}

void ShareUserLine::slotDeleteAnimationFinished()
{
    emit resizeRequested();
    emit visualDeletionDone();
    deleteLater();

    // There is a painting bug where a small line of this widget isn't
    // properly cleared. This explicit repaint() call makes sure any trace of
    // the share widget is removed once it's destroyed. #4189
    connect(this, SIGNAL(destroyed(QObject *)), parentWidget(), SLOT(repaint()));
}

void ShareUserLine::refreshPasswordOptions()
{
    const bool isPasswordEnabled = _share->getShareType() == Share::TypeEmail && _passwordProtectLinkAction->isChecked();

    _ui->passwordLabel->setVisible(isPasswordEnabled);
    _ui->lineEdit_password->setEnabled(isPasswordEnabled);
    _ui->lineEdit_password->setVisible(isPasswordEnabled);
    _ui->confirmPassword->setVisible(isPasswordEnabled);

    emit resizeRequested();
}

void ShareUserLine::refreshPasswordLineEditPlaceholder()
{
    if (_share->isPasswordSet()) {
        _ui->lineEdit_password->setPlaceholderText(QString::fromUtf8(passwordIsSetPlaceholder));
    } else {
        _ui->lineEdit_password->setPlaceholderText("");
    }
}

void ShareUserLine::slotPasswordSet()
{
    togglePasswordSetProgressAnimation(false);
    _ui->lineEdit_password->setEnabled(true);
    _ui->confirmPassword->setEnabled(true);

    _ui->lineEdit_password->setText("");

    _passwordProtectLinkAction->setEnabled(!_share->isPasswordSet() || !_account->capabilities().shareEmailPasswordEnforced());

    refreshPasswordLineEditPlaceholder();

    refreshPasswordOptions();
}

void ShareUserLine::slotPasswordSetError(int statusCode, const QString &message)
{
    qCWarning(lcSharing) << "Error from server" << statusCode << message;

    togglePasswordSetProgressAnimation(false);

    _ui->lineEdit_password->setEnabled(true);
    _ui->confirmPassword->setEnabled(true);

    refreshPasswordLineEditPlaceholder();

    refreshPasswordOptions();

    focusPasswordLineEdit();

    _ui->errorLabel->show();
    _ui->errorLabel->setText(message);

    emit resizeRequested();
}

void ShareUserLine::slotShareDeleted()
{
    auto *animation = new QPropertyAnimation(this, "maximumHeight", this);

    animation->setDuration(500);
    animation->setStartValue(height());
    animation->setEndValue(0);

    connect(animation, &QAbstractAnimation::finished, this, &ShareUserLine::slotDeleteAnimationFinished);
    connect(animation, &QVariantAnimation::valueChanged, this, &ShareUserLine::resizeRequested);

    animation->start();
}

void ShareUserLine::slotPermissionsSet()
{
    displayPermissions();
    setEnabled(true);
}

QSharedPointer<Share> ShareUserLine::share() const
{
    return _share;
}

void ShareUserLine::displayPermissions()
{
    auto perm = _share->getPermissions();

//  folders edit = CREATE, READ, UPDATE, DELETE
//  files edit = READ + UPDATE
    if (perm & SharePermissionUpdate && (_isFile ||
                                         (perm & SharePermissionCreate && perm & SharePermissionDelete))) {
        _ui->permissionsEdit->setCheckState(Qt::Checked);
    } else if (!_isFile && perm & (SharePermissionUpdate | SharePermissionCreate | SharePermissionDelete)) {
        _ui->permissionsEdit->setCheckState(Qt::PartiallyChecked);
    } else if(perm & SharePermissionRead) {
        _ui->permissionsEdit->setCheckState(Qt::Unchecked);
    }

//  edit is independent of reshare
    if (perm & SharePermissionShare)
        _permissionReshare->setChecked(true);

    if(!_isFile){
        _permissionCreate->setChecked(perm & SharePermissionCreate);
        _permissionChange->setChecked(perm & SharePermissionUpdate);
        _permissionDelete->setChecked(perm & SharePermissionDelete);
    }
}

void ShareUserLine::slotStyleChanged()
{
    customizeStyle();
}

void ShareUserLine::focusPasswordLineEdit()
{
    _ui->lineEdit_password->setFocus();
}

void ShareUserLine::customizeStyle()
{
    _ui->permissionToolButton->setIcon(Theme::createColorAwareIcon(":/client/theme/more.svg"));

    QIcon deleteicon = QIcon::fromTheme(QLatin1String("user-trash"),Theme::createColorAwareIcon(QLatin1String(":/client/theme/delete.svg")));
    _deleteShareButton->setIcon(deleteicon);

    _ui->noteConfirmButton->setIcon(Theme::createColorAwareIcon(":/client/theme/confirm.svg"));
    _ui->confirmExpirationDate->setIcon(Theme::createColorAwareIcon(":/client/theme/confirm.svg"));
    _ui->progressIndicator->setColor(QGuiApplication::palette().color(QPalette::WindowText));

    // make sure to force BackgroundRole to QPalette::WindowText for a lable, because it's parent always has a different role set that applies to children unless customized
    _ui->errorLabel->setBackgroundRole(QPalette::WindowText);
}

void ShareUserLine::showNoteOptions(bool show)
{
    _ui->noteLabel->setVisible(show);
    _ui->noteTextEdit->setVisible(show);
    _ui->noteConfirmButton->setVisible(show);

    if (show) {
        const auto note = _share->getNote();
        _ui->noteTextEdit->setText(note);
        _ui->noteTextEdit->setFocus();
    }

    emit resizeRequested();
}


void ShareUserLine::toggleNoteOptions(bool enable)
{
    showNoteOptions(enable);

    if (!enable) {
        // Delete note
        _share->setNote(QString());
    }
}

void ShareUserLine::onNoteConfirmButtonClicked()
{
    setNote(_ui->noteTextEdit->toPlainText());
}

void ShareUserLine::setNote(const QString &note)
{
    enableProgessIndicatorAnimation(true);
    _share->setNote(note);
}

void ShareUserLine::toggleExpireDateOptions(bool enable)
{
    showExpireDateOptions(enable);

    if (!enable) {
        _share->setExpireDate(QDate());
    }
}

void ShareUserLine::showExpireDateOptions(bool show)
{
    _ui->expirationLabel->setVisible(show);
    _ui->calendar->setVisible(show);
    _ui->confirmExpirationDate->setVisible(show);

    if (show) {
        const QDate date = QDate::currentDate().addDays(1);
        _ui->calendar->setDate(date);
        _ui->calendar->setMinimumDate(date);
        _ui->calendar->setFocus();
    }

    emit resizeRequested();
}

void ShareUserLine::setExpireDate()
{
    enableProgessIndicatorAnimation(true);
    _share->setExpireDate(_ui->calendar->date());
}

void ShareUserLine::enableProgessIndicatorAnimation(bool enable)
{
    if (enable) {
        if (!_ui->progressIndicator->isAnimated()) {
            _ui->progressIndicator->startAnimation();
        }
    } else {
        _ui->progressIndicator->stopAnimation();
    }
}

void ShareUserLine::togglePasswordSetProgressAnimation(bool show)
{
    // button and progress indicator are interchanged depending on if the network request is in progress or not
    _ui->confirmPassword->setVisible(!show && _passwordProtectLinkAction->isChecked());
    _ui->passwordProgressIndicator->setVisible(show);
    if (show) {
        if (!_ui->passwordProgressIndicator->isAnimated()) {
            _ui->passwordProgressIndicator->startAnimation();
        }
    } else {
        _ui->passwordProgressIndicator->stopAnimation();
    }
}

void ShareUserLine::disableProgessIndicatorAnimation()
{
    enableProgessIndicatorAnimation(false);
}

void ShareUserLine::setPasswordConfirmed()
{
    if (_ui->lineEdit_password->text().isEmpty()) {
        return;
    }

    _ui->lineEdit_password->setEnabled(false);
    _ui->confirmPassword->setEnabled(false);

    _ui->errorLabel->hide();
    _ui->errorLabel->clear();

    togglePasswordSetProgressAnimation(true);
    _share->setPassword(_ui->lineEdit_password->text());
}

void ShareUserLine::slotLineEditPasswordReturnPressed()
{
    setPasswordConfirmed();
}

void ShareUserLine::slotConfirmPasswordClicked()
{
    setPasswordConfirmed();
}
}
