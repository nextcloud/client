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

#ifndef SETTINGSDIALOG_H
#define SETTINGSDIALOG_H

#include <QDialog>
#include <QStyledItemDelegate>

#include "progressdispatcher.h"
#include "owncloudgui.h"

class QAction;
class QActionGroup;
class QToolBar;
class QStandardItemModel;

namespace OCC {

class AccountState;

namespace Ui {
    class SettingsDialog;
}
class AccountSettings;
class Application;
class FolderMan;
class ownCloudGui;

/**
 * @brief The SettingsDialog class
 * @ingroup gui
 */
class SettingsDialog : public QDialog
{
    Q_OBJECT

public:
    explicit SettingsDialog(ownCloudGui *gui, QWidget *parent = nullptr);
    ~SettingsDialog();

    void addAccount(const QString &title, QWidget *widget);

public slots:
    void showFirstPage();
    void showIssuesList(AccountState *account);
    void slotSwitchPage(QAction *action);
    void slotAccountAvatarChanged();
    void slotAccountDisplayNameChanged();

signals:
    void styleChanged();
    void onActivate();
    void onSetVisible(bool visible);

protected:
    void reject() override;
    void accept() override;
    void changeEvent(QEvent *) override;
    void setVisible(bool visible) override;

private slots:
    void accountAdded(AccountState *);
    void accountRemoved(AccountState *);

private:
    void customizeStyle();

    QAction *createColorAwareAction(const QString &iconName, const QString &fileName);
    QAction *createActionWithIcon(const QIcon &icon, const QString &text, const QString &iconPath = QString());

    Ui::SettingsDialog *const _ui;

    QActionGroup *_actionGroup;
    QAction *_actionBefore;
    // Maps the actions from the action group to the corresponding widgets
    QHash<QAction *, QWidget *> _actionGroupWidgets;

    // Maps the action in the dialog to their according account. Needed in
    // case the account avatar changes
    QHash<Account *, QAction *> _actionForAccount;

    QToolBar *_toolBar;

    ownCloudGui *_gui;
};
}

#endif // SETTINGSDIALOG_H
