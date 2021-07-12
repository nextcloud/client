/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 * Copyright (C) by Julius Härtl <jus@bitgrid.net>
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

#include <glib.h>
#include <gio/gio.h>
#include <cloudprovidersaccountexporter.h>
#include <cloudprovidersproviderexporter.h>

#include "cloudproviderwrapper.h"
#include <account.h>
#include <folder.h>
#include <accountstate.h>
#include <QDesktopServices>
#include "openfilemanager.h"
#include "owncloudgui.h"
#include "application.h"

using namespace OCC;

GSimpleActionGroup *actionGroup = nullptr;

CloudProviderWrapper::CloudProviderWrapper(QObject *parent, Folder *folder, int folderId, CloudProvidersProviderExporter* cloudprovider) : QObject(parent)
  , _folder(folder)
{
    GMenuModel *model;
    GActionGroup *action_group;
    QString accountName = QString("Folder/%1").arg(folderId);

    _cloudProvider = CLOUD_PROVIDERS_PROVIDER_EXPORTER(cloudprovider);
    _cloudProviderAccount = cloud_providers_account_exporter_new(_cloudProvider, accountName.toUtf8().data());

    cloud_providers_account_exporter_set_name (_cloudProviderAccount, folder->shortGuiLocalPath().toUtf8().data());
    cloud_providers_account_exporter_set_icon (_cloudProviderAccount, g_icon_new_for_string(APPLICATION_ICON_NAME, nullptr));
    cloud_providers_account_exporter_set_path (_cloudProviderAccount, folder->cleanPath().toUtf8().data());
    cloud_providers_account_exporter_set_status (_cloudProviderAccount, CLOUD_PROVIDERS_ACCOUNT_STATUS_IDLE);
    model = getMenuModel();
    cloud_providers_account_exporter_set_menu_model (_cloudProviderAccount, model);
    action_group = getActionGroup();
    cloud_providers_account_exporter_set_action_group (_cloudProviderAccount, action_group);

    connect(ProgressDispatcher::instance(), SIGNAL(progressInfo(QString, ProgressInfo)), this, SLOT(slotUpdateProgress(QString, ProgressInfo)));
    connect(_folder, SIGNAL(syncStarted()), this, SLOT(slotSyncStarted()));
    connect(_folder, SIGNAL(syncFinished(SyncResult)), this, SLOT(slotSyncFinished(const SyncResult)));
    connect(_folder, SIGNAL(syncPausedChanged(Folder*,bool)), this, SLOT(slotSyncPausedChanged(Folder*, bool)));

    _paused = _folder->syncPaused();
    updatePauseStatus();
    g_clear_object (&model);
    g_clear_object (&action_group);
}

CloudProviderWrapper::~CloudProviderWrapper()
{
    g_object_unref(_cloudProviderAccount);
    g_object_unref(_mainMenu);
    g_object_unref(actionGroup);
    g_object_unref(_recentMenu);
}

CloudProvidersAccountExporter* CloudProviderWrapper::accountExporter()
{
    return _cloudProviderAccount;
}

static bool shouldShowInRecentsMenu(const SyncFileItem &item)
{
    return !Progress::isIgnoredKind(item._status)
            && item._instruction != CSYNC_INSTRUCTION_EVAL
            && item._instruction != CSYNC_INSTRUCTION_NONE;
}

static GMenuItem *menu_item_new(const QString &label, const gchar *detailed_action)
{
    return g_menu_item_new(label.toUtf8 ().data(), detailed_action);
}

static GMenuItem *menu_item_new_submenu(const QString &label, GMenuModel *submenu)
{
    return g_menu_item_new_submenu(label.toUtf8 ().data(), submenu);
}

void CloudProviderWrapper::slotUpdateProgress(const QString &folder, const ProgressInfo &progress)
{
    // Only update progress for the current folder
    Folder *f = FolderMan::instance()->folder(folder);
    if (f != _folder)
        return;

    // Build recently changed files list
    if (!progress._lastCompletedItem.isEmpty() && shouldShowInRecentsMenu(progress._lastCompletedItem)) {
        QString kindStr = Progress::asResultString(progress._lastCompletedItem);
        QString timeStr = QTime::currentTime().toString("hh:mm");
        QString actionText = tr("%1 (%2, %3)").arg(progress._lastCompletedItem._file, kindStr, timeStr);
        if (f) {
            QString fullPath = f->path() + '/' + progress._lastCompletedItem._file;
            if (QFile(fullPath).exists()) {
                if (_recentlyChanged.length() > 5)
                    _recentlyChanged.removeFirst();
                _recentlyChanged.append(qMakePair(actionText, fullPath));
            } else {
                _recentlyChanged.append(qMakePair(actionText, QString("")));
            }
        }

    }

    // Build status details text
    QString msg;
    if (!progress._currentDiscoveredRemoteFolder.isEmpty()) {
        msg =  tr("Checking for changes in \"%1\"").arg(progress._currentDiscoveredRemoteFolder);
    } else if (progress.totalSize() == 0) {
        qint64 currentFile = progress.currentFile();
        qint64 totalFileCount = qMax(progress.totalFiles(), currentFile);
        if (progress.trustEta()) {
            msg = tr("Syncing %1 of %2  (%3 left)")
                    .arg(currentFile)
                    .arg(totalFileCount)
                    .arg(Utility::durationToDescriptiveString2(progress.totalProgress().estimatedEta));
        } else {
            msg = tr("Syncing %1 of %2")
                    .arg(currentFile)
                    .arg(totalFileCount);
        }
    } else {
        QString totalSizeStr = Utility::octetsToString(progress.totalSize());
        if (progress.trustEta()) {
            msg = tr("Syncing %1 (%2 left)")
                    .arg(totalSizeStr, Utility::durationToDescriptiveString2(progress.totalProgress().estimatedEta));
        } else {
            msg = tr("Syncing %1")
                    .arg(totalSizeStr);
        }
    }
    updateStatusText(msg);

    if (!progress._lastCompletedItem.isEmpty()
            && shouldShowInRecentsMenu(progress._lastCompletedItem)) {
        GMenuItem* item;
        g_menu_remove_all (G_MENU(_recentMenu));
        if(!_recentlyChanged.isEmpty()) {
            QList<QPair<QString, QString>>::iterator i;
            for (i = _recentlyChanged.begin(); i != _recentlyChanged.end(); i++) {
                QString label = i->first;
                QString fullPath = i->second;
                item = menu_item_new(label, "cloudprovider.showfile");
                g_menu_item_set_action_and_target_value(item, "cloudprovider.showfile", g_variant_new_string(fullPath.toUtf8().data()));
                g_menu_append_item(_recentMenu, item);
                g_clear_object (&item);
            }
        } else {
            item = menu_item_new(tr("No recently changed files"), nullptr);
            g_menu_append_item(_recentMenu, item);
            g_clear_object (&item);
        }
    }
}

void CloudProviderWrapper::updateStatusText(QString statusText)
{
    QString status = QString("%1 - %2").arg(_folder->accountState()->stateString(_folder->accountState()->state()), statusText);
    cloud_providers_account_exporter_set_status_details(_cloudProviderAccount, status.toUtf8().data());
}

void CloudProviderWrapper::updatePauseStatus()
{
    if (_paused) {
        updateStatusText(tr("Sync paused"));
        cloud_providers_account_exporter_set_status (_cloudProviderAccount, CLOUD_PROVIDERS_ACCOUNT_STATUS_ERROR);
    } else {
        updateStatusText(tr("Syncing"));
        cloud_providers_account_exporter_set_status (_cloudProviderAccount, CLOUD_PROVIDERS_ACCOUNT_STATUS_SYNCING);
    }
}

Folder* CloudProviderWrapper::folder()
{
    return _folder;
}

void CloudProviderWrapper::slotSyncStarted()
{
    cloud_providers_account_exporter_set_status(_cloudProviderAccount, CLOUD_PROVIDERS_ACCOUNT_STATUS_SYNCING);
}

void CloudProviderWrapper::slotSyncFinished(const SyncResult &result)
{
    if (result.status() == result.Success || result.status() == result.Problem)
    {
        cloud_providers_account_exporter_set_status(_cloudProviderAccount, CLOUD_PROVIDERS_ACCOUNT_STATUS_IDLE);
        updateStatusText(result.statusString());
        return;
    }
    cloud_providers_account_exporter_set_status(_cloudProviderAccount, CLOUD_PROVIDERS_ACCOUNT_STATUS_ERROR);
    updateStatusText(result.statusString());
}

GMenuModel* CloudProviderWrapper::getMenuModel() {

    GMenu* section;
    GMenuItem* item;
    QString item_label;

    _mainMenu = g_menu_new();

    section = g_menu_new();
    item = menu_item_new(tr("Open website"), "cloudprovider.openwebsite");
    g_menu_append_item(section, item);
    g_clear_object (&item);
    g_menu_append_section(_mainMenu, nullptr, G_MENU_MODEL(section));
    g_clear_object (&section);

    _recentMenu = g_menu_new();
    item = menu_item_new(tr("No recently changed files"), nullptr);
    g_menu_append_item(_recentMenu, item);
    g_clear_object (&item);

    section = g_menu_new();
    item = menu_item_new_submenu(tr("Recently changed"), G_MENU_MODEL(_recentMenu));
    g_menu_append_item(section, item);
    g_clear_object (&item);
    g_menu_append_section(_mainMenu, nullptr, G_MENU_MODEL(section));
    g_clear_object (&section);

    section = g_menu_new();
    item = menu_item_new(tr("Pause synchronization"), "cloudprovider.pause");
    g_menu_append_item(section, item);
    g_clear_object (&item);
    g_menu_append_section(_mainMenu, nullptr, G_MENU_MODEL(section));
    g_clear_object (&section);

    section = g_menu_new();
    item = menu_item_new(tr("Help"), "cloudprovider.openhelp");
    g_menu_append_item(section, item);
    g_clear_object (&item);
    item = menu_item_new(tr("Settings"), "cloudprovider.opensettings");
    g_menu_append_item(section, item);
    g_clear_object (&item);
    item = menu_item_new(tr("Log out"), "cloudprovider.logout");
    g_menu_append_item(section, item);
    g_clear_object (&item);
    item = menu_item_new(tr("Quit sync client"), "cloudprovider.quit");
    g_menu_append_item(section, item);
    g_clear_object (&item);
    g_menu_append_section(_mainMenu, nullptr, G_MENU_MODEL(section));
    g_clear_object (&section);

    return G_MENU_MODEL(_mainMenu);
}

static void
activate_action_open (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    Q_UNUSED(parameter);
    const gchar *name = g_action_get_name(G_ACTION(action));
    auto *self = static_cast<CloudProviderWrapper*>(user_data);
    auto *gui = dynamic_cast<ownCloudGui*>(self->parent()->parent());

    if(g_str_equal(name, "openhelp")) {
        gui->slotHelp();
    }

    if(g_str_equal(name, "opensettings")) {
        gui->slotShowSettings();
    }

    if(g_str_equal(name, "openwebsite")) {
        QDesktopServices::openUrl(self->folder()->accountState()->account()->url());
    }

    if(g_str_equal(name, "openfolder")) {
        showInFileManager(self->folder()->cleanPath());
    }

    if(g_str_equal(name, "showfile")) {
        const gchar *path = g_variant_get_string(parameter, nullptr);
        g_print("showfile => %s\n", path);
        showInFileManager(QString(path));
    }

    if(g_str_equal(name, "logout")) {
        self->folder()->accountState()->signOutByUi();
    }

    if(g_str_equal(name, "quit")) {
        qApp->quit();
    }
}

static void
activate_action_openrecentfile (GSimpleAction *action, GVariant *parameter, gpointer user_data)
{
    Q_UNUSED(action);
    Q_UNUSED(parameter);
    auto *self = static_cast<CloudProviderWrapper*>(user_data);
    QDesktopServices::openUrl(self->folder()->accountState()->account()->url());
}

static void
activate_action_pause (GSimpleAction *action,
                       GVariant      *parameter,
                       gpointer       user_data)
{
    Q_UNUSED(parameter);
    auto *self = static_cast<CloudProviderWrapper*>(user_data);
    GVariant *old_state, *new_state;

    old_state = g_action_get_state (G_ACTION (action));
    new_state = g_variant_new_boolean (!(bool)g_variant_get_boolean (old_state));
    self->folder()->setSyncPaused((bool)g_variant_get_boolean(new_state));
    g_simple_action_set_state (action, new_state);
    g_variant_unref (old_state);
}

static GActionEntry actions[] = {
    { "openwebsite",  activate_action_open, nullptr, nullptr, nullptr, {0,0,0}},
    { "quit",  activate_action_open, nullptr, nullptr, nullptr, {0,0,0}},
    { "logout",  activate_action_open, nullptr, nullptr, nullptr, {0,0,0}},
    { "openfolder",  activate_action_open, nullptr, nullptr, nullptr, {0,0,0}},
    { "showfile",  activate_action_open, "s", nullptr, nullptr, {0,0,0}},
    { "openhelp",  activate_action_open, nullptr, nullptr, nullptr, {0,0,0}},
    { "opensettings",  activate_action_open, nullptr, nullptr, nullptr, {0,0,0}},
    { "openrecentfile",  activate_action_openrecentfile, "s", nullptr, nullptr, {0,0,0}},
    { "pause",  activate_action_pause, nullptr, "false", nullptr, {0,0,0}}
};

GActionGroup* CloudProviderWrapper::getActionGroup()
{
    g_clear_object (&actionGroup);
    actionGroup = g_simple_action_group_new ();
    g_action_map_add_action_entries (G_ACTION_MAP (actionGroup), actions, G_N_ELEMENTS (actions), this);
    bool state = _folder->syncPaused();
    GAction *pause = g_action_map_lookup_action(G_ACTION_MAP(actionGroup), "pause");
    g_simple_action_set_state(G_SIMPLE_ACTION(pause), g_variant_new_boolean(state));
    return G_ACTION_GROUP (g_object_ref (actionGroup));
}

void CloudProviderWrapper::slotSyncPausedChanged(Folder *folder, bool state)
{
    Q_UNUSED(folder);
    _paused = state;
    GAction *pause = g_action_map_lookup_action(G_ACTION_MAP(actionGroup), "pause");
    g_simple_action_set_state (G_SIMPLE_ACTION(pause), g_variant_new_boolean(state));
    updatePauseStatus();
}
