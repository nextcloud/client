/*
 *
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
#include <QtGlobal>

#include <cmath>
#include <csignal>

#ifdef Q_OS_UNIX
#include <sys/time.h>
#include <sys/resource.h>
#endif

#include "application.h"
#include "theme.h"
#include "common/utility.h"
#include "cocoainitializer.h"

#if defined(BUILD_UPDATER)
#include "updater/updater.h"
#endif

#include <QTimer>
#include <QMessageBox>
#include <QDebug>
#include <QQuickStyle>
#include <QQuickWindow>

using namespace OCC;

void warnSystray()
{
    QMessageBox::critical(nullptr, qApp->translate("main.cpp", "System Tray not available"),
        qApp->translate("main.cpp", "%1 requires on a working system tray. "
                                    "If you are running XFCE, please follow "
                                    "<a href=\"http://docs.xfce.org/xfce/xfce4-panel/systray\">these instructions</a>. "
                                    "Otherwise, please install a system tray application such as \"trayer\" and try again.")
            .arg(Theme::instance()->appNameGUI()));
}

int main(int argc, char **argv)
{
    Q_INIT_RESOURCE(resources);
    Q_INIT_RESOURCE(theme);

    // Work around a bug in KDE's qqc2-desktop-style which breaks
    // buttons with icons not based on a name, by forcing a style name
    // the platformtheme plugin won't try to force qqc2-desktops-style
    // anymore.
    // Can be removed once the bug in qqc2-desktop-style is gone.
    QQuickStyle::setStyle("Default");

    // OpenSSL 1.1.0: No explicit initialisation or de-initialisation is necessary.

    QCoreApplication::setAttribute(Qt::AA_UseHighDpiPixmaps, true);
    QCoreApplication::setAttribute(Qt::AA_EnableHighDpiScaling, true);
#ifdef Q_OS_MAC
    Mac::CocoaInitializer cocoaInit; // RIIA
#endif
    OCC::Application app(argc, argv);

#ifdef Q_OS_WIN
    // The Windows style still has pixelated elements with Qt 5.6,
    // it's recommended to use the Fusion style in this case, even
    // though it looks slightly less native. Check here after the
    // QApplication was constructed, but before any QWidget is
    // constructed.
    if (app.devicePixelRatio() > 1)
        QApplication::setStyle(QStringLiteral("fusion"));
#endif // Q_OS_WIN

#ifndef Q_OS_WIN
    signal(SIGPIPE, SIG_IGN);
#endif

#if QT_VERSION >= QT_VERSION_CHECK(5, 14, 0)
    QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
#else
    // See https://bugreports.qt.io/browse/QTBUG-70481
    if (std::fmod(app.devicePixelRatio(), 1) == 0) {
        QQuickWindow::setTextRenderType(QQuickWindow::NativeTextRendering);
    }
#endif

// check a environment variable for core dumps
#ifdef Q_OS_UNIX
    if (!qEnvironmentVariableIsEmpty("OWNCLOUD_CORE_DUMP")) {
        struct rlimit core_limit;
        core_limit.rlim_cur = RLIM_INFINITY;
        core_limit.rlim_max = RLIM_INFINITY;

        if (setrlimit(RLIMIT_CORE, &core_limit) < 0) {
            fprintf(stderr, "Unable to set core dump limit\n");
        } else {
            qCInfo(lcApplication) << "Core dumps enabled";
        }
    }
#endif

#if defined(BUILD_UPDATER)
    // if handleStartup returns true, main()
    // needs to terminate here, e.g. because
    // the updater is triggered
    Updater *updater = Updater::instance();
    if (updater && updater->handleStartup()) {
        return 1;
    }
#endif

    // if the application is already running, notify it.
    if (app.isRunning()) {
        qCInfo(lcApplication) << "Already running, exiting...";
        if (app.isSessionRestored()) {
            // This call is mirrored with the one in Application::slotParseMessage
            qCInfo(lcApplication) << "Session was restored, don't notify app!";
            return -1;
        }

        QStringList args = app.arguments();
        if (args.size() > 1) {
            QString msg = args.join(QLatin1String("|"));
            if (!app.sendMessage(QLatin1String("MSG_PARSEOPTIONS:") + msg))
                return -1;
        } else if (!app.backgroundMode() && !app.sendMessage(QLatin1String("MSG_SHOWMAINDIALOG"))) {
            return -1;
        }
        return 0;
    }

    // We can't call isSystemTrayAvailable with appmenu-qt5 begause it hides the systemtray
    // (issue #4693)
    if (qgetenv("QT_QPA_PLATFORMTHEME") != "appmenu-qt5")
    {
        if (!QSystemTrayIcon::isSystemTrayAvailable()) {
            // If the systemtray is not there, we will wait one second for it to maybe start
            // (eg boot time) then we show the settings dialog if there is still no systemtray.
            // On XFCE however, we show a message box with explainaition how to install a systemtray.
            qCInfo(lcApplication) << "System tray is not available, waiting...";
            Utility::sleep(1);

            auto desktopSession = qgetenv("XDG_CURRENT_DESKTOP").toLower();
            if (desktopSession.isEmpty()) {
                desktopSession = qgetenv("DESKTOP_SESSION").toLower();
            }
            if (desktopSession == "xfce") {
                int attempts = 0;
                while (!QSystemTrayIcon::isSystemTrayAvailable()) {
                    attempts++;
                    if (attempts >= 30) {
                        qCWarning(lcApplication) << "System tray unavailable (xfce)";
                        warnSystray();
                        break;
                    }
                    Utility::sleep(1);
                }
            }

            if (QSystemTrayIcon::isSystemTrayAvailable()) {
                app.tryTrayAgain();
            } else if (!app.backgroundMode() && !AccountManager::instance()->accounts().isEmpty()) {
                if (desktopSession != "ubuntu") {
                    qCInfo(lcApplication) << "System tray still not available, showing window and trying again later";
                    app.showMainDialog();
                    QTimer::singleShot(10000, &app, &Application::tryTrayAgain);
                } else {
                    qCInfo(lcApplication) << "System tray still not available, but assuming it's fine on 'ubuntu' desktop";
                }
            }
        }
    }

    return app.exec();
}
