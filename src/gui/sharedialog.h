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

#ifndef SHAREDIALOG_H
#define SHAREDIALOG_H

#include "accountstate.h"
#include "sharepermissions.h"
#include "owncloudgui.h"

#include <QSharedPointer>
#include <QPointer>
#include <QString>
#include <QDialog>
#include <QWidget>

class QProgressIndicator;

namespace OCC {

namespace Ui {
    class ShareDialog;
}

class ShareLinkWidget;
class ShareUserGroupWidget;
class ShareManager;
class LinkShare;
class Share;

class ShareDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ShareDialog(QPointer<AccountState> accountState,
        const QString &sharePath,
        const QString &localPath,
        SharePermissions maxSharingPermissions,
        const QByteArray &numericFileId,
        ShareDialogStartPage startPage,
        QWidget *parent = nullptr);
    ~ShareDialog();

private slots:
    void done(int r) override;
    void slotPropfindReceived(const QVariantMap &result);
    void slotPropfindError();
    void slotThumbnailFetched(const int &statusCode, const QByteArray &reply);
    void slotAccountStateChanged(int state);

    void slotSharesFetched(const QList<QSharedPointer<Share>> &shares);
    void slotAddLinkShareWidget(const QSharedPointer<LinkShare> &linkShare);
    void slotDeleteShare();
    void slotCreateLinkShare();
    void slotLinkShareRequiresPassword();
    void slotAdjustScrollWidgetSize();

signals:
    void toggleAnimation(bool);
    void styleChanged();
    void onSetVisible(bool visible);

protected:
    void changeEvent(QEvent *) override;
    void setVisible(bool visible) override;

private:
    void showSharingUi();
    void addLinkShareWidget(const QSharedPointer<LinkShare> &linkShare);
    void initLinkShareWidget();

    Ui::ShareDialog *_ui;

    QPointer<AccountState> _accountState;
    QString _sharePath;
    QString _localPath;
    SharePermissions _maxSharingPermissions;
    QByteArray _numericFileId;
    QString _privateLinkUrl;
    ShareDialogStartPage _startPage;
    ShareManager *_manager = nullptr;

    QList<ShareLinkWidget*> _linkWidgetList;
    ShareLinkWidget* _emptyShareLinkWidget = nullptr;
    ShareUserGroupWidget *_userGroupWidget = nullptr;
    QProgressIndicator *_progressIndicator = nullptr;
};

} // namespace OCC

#endif // SHAREDIALOG_H
