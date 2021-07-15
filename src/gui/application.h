/*
 * Copyright (C) by Duncan Mac-Vicar P. <duncan@kde.org>
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

#ifndef APPLICATION_H
#define APPLICATION_H

#include <QApplication>
#include <QCommandLineParser>
#include <QPointer>
#include <QQueue>
#include <QTimer>
#include <QElapsedTimer>
#include <QNetworkConfigurationManager>

#include "qtsingleapplication.h"

#include "syncresult.h"
#include "logbrowser.h"
#include "owncloudgui.h"
#include "connectionvalidator.h"
#include "progressdispatcher.h"
#include "clientproxy.h"
#include "folderman.h"

class QMessageBox;
class QSystemTrayIcon;
class QSocket;

namespace CrashReporter {
class Handler;
}

namespace OCC {

Q_DECLARE_LOGGING_CATEGORY(lcApplication)

class Theme;
class Folder;
class SslErrorDialog;

/**
 * @brief The Application class
 * @ingroup gui
 */
class Application : public SharedTools::QtSingleApplication
{
    Q_OBJECT
public:
    explicit Application(int &argc, char **argv);
    ~Application();

    void showHint(std::string errorHint);
    bool backgroundMode() const;

    void showMainDialog();

    ownCloudGui *gui() const;

public slots:
    // TODO: this should not be public
    void slotownCloudWizardDone(int);
    void slotCrash();
    /**
     * Will download a virtual file, and open the result.
     * The argument is the filename of the virtual file (including the extension)
     */
    void openVirtualFile(const QString &filename);

    /// Attempt to show() the tray icon again. Used if no systray was available initially.
    void tryTrayAgain();

protected:
    void setupTranslations();
    void setupLogging();
    bool event(QEvent *event);

signals:
    void folderRemoved();
    void folderStateChanged(Folder *);
    void isShowingSettingsDialog();

protected slots:
    void slotParseMessage(const QString &, QObject *);
    void slotCheckConnection();
    void slotUseMonoIconsChanged(bool);
    void slotCleanup();
    void slotAccountStateAdded(AccountState *accountState);
    void slotAccountStateRemoved(AccountState *accountState);
    void slotSystemOnlineConfigurationChanged(QNetworkConfiguration);
    void slotGuiIsShowingSettings();

private:
    void virtualFileFromArguments();

    /**
     * Maybe a newer version of the client was used with this config file:
     * if so, backup, confirm with user and remove the config that can't be read.
     */
    bool configVersionMigration();

    QPointer<ownCloudGui> _gui;
    QCommandLineParser _parser;
    Theme *_theme;
    QElapsedTimer _startedAt;
    ClientProxy _proxy;
    QNetworkConfigurationManager _networkConfigurationManager;
    QTimer _checkConnectionTimer;

#if defined(WITH_CRASHREPORTER)
    QScopedPointer<CrashReporter::Handler> _crashHandler;
#endif
    QScopedPointer<FolderMan> _folderManager;
};

} // namespace OCC

#endif // APPLICATION_H
