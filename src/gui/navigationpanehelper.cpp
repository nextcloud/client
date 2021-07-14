/*
 * Copyright (C) by Jocelyn Turcotte <jturcotte@woboq.com>
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

#include "navigationpanehelper.h"
#include "accountmanager.h"
#include "configfile.h"
#include "folderman.h"

#include <QDir>
#include <QCoreApplication>

namespace OCC {

Q_LOGGING_CATEGORY(lcNavPane, "nextcloud.gui.folder.navigationpane", QtInfoMsg)

NavigationPaneHelper::NavigationPaneHelper(FolderMan *folderMan)
    : _folderMan(folderMan)
{
    ConfigFile cfg;
    _showInExplorerNavigationPane = cfg.showInExplorerNavigationPane();

    _updateCloudStorageRegistryTimer.setSingleShot(true);
    connect(&_updateCloudStorageRegistryTimer, &QTimer::timeout, this, &NavigationPaneHelper::updateCloudStorageRegistry);

    // Ensure that the folder integration stays persistent in Explorer,
    // the uninstaller removes the folder upon updating the client.
    _showInExplorerNavigationPane = !_showInExplorerNavigationPane;
    setShowInExplorerNavigationPane(!_showInExplorerNavigationPane);
}

void NavigationPaneHelper::setShowInExplorerNavigationPane(bool show)
{
    if (_showInExplorerNavigationPane == show)
        return;

    _showInExplorerNavigationPane = show;
    // Re-generate a new CLSID when enabling, possibly throwing away the old one.
    // updateCloudStorageRegistry will take care of removing any unknown CLSID our application owns from the registry.
    foreach (Folder *folder, _folderMan->map())
        folder->setNavigationPaneClsid(show ? QUuid::createUuid() : QUuid());

    scheduleUpdateCloudStorageRegistry();
}

void NavigationPaneHelper::scheduleUpdateCloudStorageRegistry()
{
    // Schedule the update to happen a bit later to avoid doing the update multiple times in a row.
    if (!_updateCloudStorageRegistryTimer.isActive())
        _updateCloudStorageRegistryTimer.start(500);
}

void NavigationPaneHelper::updateCloudStorageRegistry()
{
    // Start by looking at every registered namespace extension for the sidebar, and look for an "ApplicationName" value
    // that matches ours when we saved.
    QVector<QUuid> entriesToRemove;
#ifdef Q_OS_WIN
    QString nameSpaceKey = QStringLiteral(R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace)");
    if (Utility::registryKeyExists(HKEY_CURRENT_USER, nameSpaceKey)) {
        Utility::registryWalkSubKeys(HKEY_CURRENT_USER, nameSpaceKey,
            [&entriesToRemove](HKEY key, const QString &subKey) {
                QVariant appName = Utility::registryGetKeyValue(key, subKey, QStringLiteral("ApplicationName"));
                if (appName.toString() == QLatin1String(APPLICATION_NAME)) {
                    QUuid clsid{ subKey };
                    Q_ASSERT(!clsid.isNull());
                    entriesToRemove.append(clsid);
                }
            });
    }
#endif

    // Only save folder entries if the option is enabled.
    if (_showInExplorerNavigationPane) {
        // Then re-save every folder that has a valid navigationPaneClsid to the registry.
        // We currently don't distinguish between new and existing CLSIDs, if it's there we just
        // save over it. We at least need to update the tile in case we are suddently using multiple accounts.
        foreach (Folder *folder, _folderMan->map()) {
            if (!folder->navigationPaneClsid().isNull()) {
                // If it already exists, unmark it for removal, this is a valid sync root.
                entriesToRemove.removeOne(folder->navigationPaneClsid());

                QString clsidStr = folder->navigationPaneClsid().toString();
                QString clsidPath = QString() % R"(Software\Classes\CLSID\)" % clsidStr;
                QString clsidPathWow64 = QString() % R"(Software\Classes\Wow6432Node\CLSID\)" % clsidStr;
                QString namespacePath = QString() % R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\)" % clsidStr;

                QString title = folder->shortGuiRemotePathOrAppName();
                // Write the account name in the sidebar only when using more than one account.
                if (AccountManager::instance()->accounts().size() > 1)
                    title = title % " - " % folder->accountState()->account()->davDisplayName() % " - " % folder->accountState()->account()->url().host();
                QString iconPath = QDir::toNativeSeparators(qApp->applicationFilePath());
                QString targetFolderPath = QDir::toNativeSeparators(folder->cleanPath());

                qCInfo(lcNavPane) << "Explorer Cloud storage provider: saving path" << targetFolderPath << "to CLSID" << clsidStr;
#ifdef Q_OS_WIN
                // Steps taken from: https://msdn.microsoft.com/en-us/library/windows/desktop/dn889934%28v=vs.85%29.aspx
                // Step 1: Add your CLSID and name your extension
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPath, QString(), REG_SZ, title);
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPathWow64, QString(), REG_SZ, title);
                // Step 2: Set the image for your icon
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPath + QStringLiteral("\\DefaultIcon"), QString(), REG_SZ, iconPath);
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPathWow64 + QStringLiteral("\\DefaultIcon"), QString(), REG_SZ, iconPath);
                // Step 3: Add your extension to the Navigation Pane and make it visible
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPath, QStringLiteral("System.IsPinnedToNameSpaceTree"), REG_DWORD, 0x1);
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPathWow64, QStringLiteral("System.IsPinnedToNameSpaceTree"), REG_DWORD, 0x1);
                // Step 4: Set the location for your extension in the Navigation Pane
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPath, QStringLiteral("SortOrderIndex"), REG_DWORD, 0x41);
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPathWow64, QStringLiteral("SortOrderIndex"), REG_DWORD, 0x41);
                // Step 5: Provide the dll that hosts your extension.
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPath + QStringLiteral("\\InProcServer32"), QString(), REG_EXPAND_SZ, QStringLiteral("%systemroot%\\system32\\shell32.dll"));
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPathWow64 + QStringLiteral("\\InProcServer32"), QString(), REG_EXPAND_SZ, QStringLiteral("%systemroot%\\system32\\shell32.dll"));
                // Step 6: Define the instance object
                // Indicate that your namespace extension should function like other file folder structures in File Explorer.
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPath + QStringLiteral("\\Instance"), QStringLiteral("CLSID"), REG_SZ, QStringLiteral("{0E5AAE11-A475-4c5b-AB00-C66DE400274E}"));
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPathWow64 + QStringLiteral("\\Instance"), QStringLiteral("CLSID"), REG_SZ, QStringLiteral("{0E5AAE11-A475-4c5b-AB00-C66DE400274E}"));
                // Step 7: Provide the file system attributes of the target folder
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPath + QStringLiteral("\\Instance\\InitPropertyBag"), QStringLiteral("Attributes"), REG_DWORD, 0x11);
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPathWow64 + QStringLiteral("\\Instance\\InitPropertyBag"), QStringLiteral("Attributes"), REG_DWORD, 0x11);
                // Step 8: Set the path for the sync root
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPath + QStringLiteral("\\Instance\\InitPropertyBag"), QStringLiteral("TargetFolderPath"), REG_SZ, targetFolderPath);
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPathWow64 + QStringLiteral("\\Instance\\InitPropertyBag"), QStringLiteral("TargetFolderPath"), REG_SZ, targetFolderPath);
                // Step 9: Set appropriate shell flags
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPath + QStringLiteral("\\ShellFolder"), QStringLiteral("FolderValueFlags"), REG_DWORD, 0x28);
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPathWow64 + QStringLiteral("\\ShellFolder"), QStringLiteral("FolderValueFlags"), REG_DWORD, 0x28);
                // Step 10: Set the appropriate flags to control your shell behavior
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPath + QStringLiteral("\\ShellFolder"), QStringLiteral("Attributes"), REG_DWORD, 0xF080004D);
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, clsidPathWow64 + QStringLiteral("\\ShellFolder"), QStringLiteral("Attributes"), REG_DWORD, 0xF080004D);
                // Step 11: Register your extension in the namespace root
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, namespacePath, QString(), REG_SZ, title);
                // Step 12: Hide your extension from the Desktop
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, QStringLiteral(R"(Software\Microsoft\Windows\CurrentVersion\Explorer\HideDesktopIcons\NewStartPanel)"), clsidStr, REG_DWORD, 0x1);

                // For us, to later be able to iterate and find our own namespace entries and associated CLSID.
                // Use the macro instead of the theme to make sure it matches with the uninstaller.
                Utility::registrySetKeyValue(HKEY_CURRENT_USER, namespacePath, QStringLiteral("ApplicationName"), REG_SZ, QLatin1String(APPLICATION_NAME));
#else
                // This code path should only occur on Windows (the config will be false, and the checkbox invisible on other platforms).
                // Add runtime checks rather than #ifdefing out the whole code to help catch breakages when developing on other platforms.

                // Don't crash, by any means!
                // Q_ASSERT(false);
#endif
            }
        }
    }

    // Then remove anything that isn't in our folder list anymore.
    foreach (auto &clsid, entriesToRemove) {
        QString clsidStr = clsid.toString();
        QString clsidPath = QString() % R"(Software\Classes\CLSID\)" % clsidStr;
        QString clsidPathWow64 = QString() % R"(Software\Classes\Wow6432Node\CLSID\)" % clsidStr;
        QString namespacePath = QString() % R"(Software\Microsoft\Windows\CurrentVersion\Explorer\Desktop\NameSpace\)" % clsidStr;

        qCInfo(lcNavPane) << "Explorer Cloud storage provider: now unused, removing own CLSID" << clsidStr;
#ifdef Q_OS_WIN
        Utility::registryDeleteKeyTree(HKEY_CURRENT_USER, clsidPath);
        Utility::registryDeleteKeyTree(HKEY_CURRENT_USER, clsidPathWow64);
        Utility::registryDeleteKeyTree(HKEY_CURRENT_USER, namespacePath);
        Utility::registryDeleteKeyValue(HKEY_CURRENT_USER, QStringLiteral("Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\HideDesktopIcons\\NewStartPanel"), clsidStr);
#endif
    }
}

} // namespace OCC
