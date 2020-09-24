/*
 * Copyright (C) by Cédric Bellegarde <gnumdk@gmail.com>
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

#include "accountmanager.h"
#include "systray.h"
#include "theme.h"
#include "config.h"
#include "common/utility.h"
#include "tray/UserModel.h"

#include <QCursor>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QScreen>
#include <QMenu>

#ifdef USE_FDO_NOTIFICATIONS
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusPendingCall>
#define NOTIFICATIONS_SERVICE "org.freedesktop.Notifications"
#define NOTIFICATIONS_PATH "/org/freedesktop/Notifications"
#define NOTIFICATIONS_IFACE "org.freedesktop.Notifications"
#endif

namespace OCC {

Q_LOGGING_CATEGORY(lcSystray, "nextcloud.gui.systray")

Systray *Systray::_instance = nullptr;

Systray *Systray::instance()
{
    if (!_instance) {
        _instance = new Systray();
    }
    return _instance;
}

void Systray::setTrayEngine(QQmlApplicationEngine *trayEngine)
{
    _trayEngine = trayEngine;

    _trayEngine->addImportPath("qrc:/qml/theme");
    _trayEngine->addImageProvider("avatars", new ImageProvider);
}

Systray::Systray()
    : QSystemTrayIcon(nullptr)
    ,_notificationList(QStringList())
{
    qmlRegisterSingletonType<UserModel>("com.nextcloud.desktopclient", 1, 0, "UserModel",
        [](QQmlEngine *, QJSEngine *) -> QObject * {
            return UserModel::instance();
        }
    );

    qmlRegisterSingletonType<UserAppsModel>("com.nextcloud.desktopclient", 1, 0, "UserAppsModel",
        [](QQmlEngine *, QJSEngine *) -> QObject * {
            return UserAppsModel::instance();
        }
    );

    qmlRegisterSingletonType<Systray>("com.nextcloud.desktopclient", 1, 0, "Systray",
        [](QQmlEngine *, QJSEngine *) -> QObject * {
            return Systray::instance();
        }
    );

#ifndef Q_OS_MAC
    auto contextMenu = new QMenu();
    if (AccountManager::instance()->accounts().isEmpty()) {
        contextMenu->addAction(tr("Add account"), this, &Systray::openAccountWizard);
    } else {
        contextMenu->addAction(tr("Open main dialog"), this, &Systray::openMainDialog);
    }
    contextMenu->addAction(tr("Settings"), this, &Systray::openSettings);
    contextMenu->addAction(tr("Exit %1").arg(Theme::instance()->appNameGUI()), this, &Systray::shutdown);
    setContextMenu(contextMenu);
#endif

    connect(UserModel::instance(), &UserModel::newUserSelected,
        this, &Systray::slotNewUserSelected);

    connect(AccountManager::instance(), &AccountManager::accountAdded,
        this, &Systray::showWindow);
}

void Systray::create()
{
    if (_trayEngine) {
        if (!AccountManager::instance()->accounts().isEmpty()) {
            _trayEngine->rootContext()->setContextProperty("activityModel", UserModel::instance()->currentActivityModel());
        }
        _trayEngine->load(QStringLiteral("qrc:/qml/src/gui/tray/Window.qml"));
    }
    hideWindow();
    emit activated(QSystemTrayIcon::ActivationReason::Unknown);

    const auto folderMap = FolderMan::instance()->map();
    for (const auto *folder : folderMap) {
        if (!folder->syncPaused()) {
            _syncIsPaused = false;
            break;
        }
    }
}

void Systray::slotNewUserSelected()
{
    if (_trayEngine) {
        // Change ActivityModel
        _trayEngine->rootContext()->setContextProperty("activityModel", UserModel::instance()->currentActivityModel());
    }

    // Rebuild App list
    UserAppsModel::instance()->buildAppList();
}

bool Systray::isOpen()
{
    return _isOpen;
}

Q_INVOKABLE void Systray::setOpened()
{
    _isOpen = true;
}

Q_INVOKABLE void Systray::setClosed()
{
    _isOpen = false;
}

void Systray::showMessage(const QString &title, const QString &message, MessageIcon icon)
{
#ifdef USE_FDO_NOTIFICATIONS
    if (QDBusInterface(NOTIFICATIONS_SERVICE, NOTIFICATIONS_PATH, NOTIFICATIONS_IFACE).isValid()) {
        const QVariantMap hints = {{QStringLiteral("desktop-entry"), LINUX_APPLICATION_ID}};
        QList<QVariant> args = QList<QVariant>() << APPLICATION_NAME << quint32(0) << APPLICATION_ICON_NAME
                                                 << title << message << QStringList() << hints << qint32(-1);
        QDBusMessage method = QDBusMessage::createMethodCall(NOTIFICATIONS_SERVICE, NOTIFICATIONS_PATH, NOTIFICATIONS_IFACE, "Notify");
        method.setArguments(args);
        QDBusConnection::sessionBus().asyncCall(method);
    } else
#endif
#ifdef Q_OS_OSX
        if (canOsXSendUserNotification()) {
        sendOsXUserNotification(title, message);
    } else
#endif
    {
        QSystemTrayIcon::showMessage(title, message, icon);
    }
}

void Systray::setToolTip(const QString &tip)
{
    QSystemTrayIcon::setToolTip(tr("%1: %2").arg(Theme::instance()->appNameGUI(), tip));
}

bool Systray::syncIsPaused()
{
    return _syncIsPaused;
}

void Systray::pauseResumeSync()
{
    if (_syncIsPaused) {
        _syncIsPaused = false;
        emit resumeSync();
    } else {
        _syncIsPaused = true;
        emit pauseSync();
    }
}

QString Systray::getLastNotification() const {
    if (!_notificationList.isEmpty()) {
        return _notificationList.last();
    } else {
        return QString();
    }
}

void Systray::dismissLastNotification() {
    if (!_notificationList.isEmpty()) {
        _notificationList.removeLast();
        emit notificationsChanged();
    }
}

/********************************************************************************************/
/* Helper functions for cross-platform tray icon position and taskbar orientation detection */
/********************************************************************************************/

void Systray::positionWindow(QQuickWindow *window) const
{
    window->setScreen(currentScreen());

    const auto position = computeWindowPosition(window->width(), window->height());
    window->setPosition(position);
}

QScreen *Systray::currentScreen() const
{
    const auto screens = QGuiApplication::screens();
    const auto cursorPos = QCursor::pos();

    for (const auto screen : screens) {
        if (screen->geometry().contains(cursorPos)) {
            return screen;
        }
    }

    // Didn't find anything matching the cursor position,
    // falling back to the primary screen
    return QGuiApplication::primaryScreen();
}

Systray::TaskBarPosition Systray::taskbarOrientation() const
{
// macOS: Always on top
#if defined(Q_OS_MACOS)
    return TaskBarPosition::Top;
// Windows: Check registry for actual taskbar orientation
#elif defined(Q_OS_WIN)
    auto taskbarPosition = Utility::registryGetKeyValue(HKEY_CURRENT_USER,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StuckRects3",
        "Settings");
    switch (taskbarPosition.toInt()) {
    // Mapping windows binary value (0 = left, 1 = top, 2 = right, 3 = bottom) to qml logic (0 = bottom, 1 = left...)
    case 0:
        return TaskBarPosition::Left;
    case 1:
        return TaskBarPosition::Top;
    case 2:
        return TaskBarPosition::Right;
    case 3:
        return TaskBarPosition::Bottom;
    default:
        return TaskBarPosition::Bottom;
    }
// Probably Linux
#else
    const auto screenRect = currentScreenRect();
    const auto trayIconCenter = calcTrayIconCenter();

    const auto distBottom = screenRect.bottom() - trayIconCenter.y();
    const auto distRight = screenRect.right() - trayIconCenter.x();
    const auto distLeft = trayIconCenter.x() - screenRect.left();
    const auto distTop = trayIconCenter.y() - screenRect.top();

    const auto minDist = std::min({distRight, distTop, distBottom});

    if (minDist == distBottom) {
        return TaskBarPosition::Bottom;
    } else if (minDist == distLeft) {
        return TaskBarPosition::Left;
    } else if (minDist == distTop) {
        return TaskBarPosition::Top;
    } else {
        return TaskBarPosition::Right;
    }
#endif
}

// TODO: Get real taskbar dimensions Linux as well
QRect Systray::taskbarGeometry() const
{
#if defined(Q_OS_WIN)
    QRect tbRect = Utility::getTaskbarDimensions();
    //QML side expects effective pixels, convert taskbar dimensions if necessary
    auto pixelRatio = currentScreen()->devicePixelRatio();
    if (pixelRatio != 1) {
        tbRect.setHeight(tbRect.height() / pixelRatio);
        tbRect.setWidth(tbRect.width() / pixelRatio);
    }
    return tbRect;
#elif defined(Q_OS_MACOS)
    // Finder bar is always 22px height on macOS (when treating as effective pixels)
    auto screenWidth = currentScreenRect().width();
    return {0, 0, screenWidth, 22};
#else
    if (taskbarOrientation() == TaskBarPosition::Bottom || taskbarOrientation() == TaskBarPosition::Top) {
        auto screenWidth = currentScreenRect().width();
        return {0, 0, screenWidth, 32};
    } else {
        auto screenHeight = currentScreenRect().height();
        return {0, 0, 32, screenHeight};
    }
#endif
}

QRect Systray::currentScreenRect() const
{
    const auto screen = currentScreen();
    Q_ASSERT(screen);
    return screen->geometry();
}

QPoint Systray::computeWindowReferencePoint() const
{
    constexpr auto spacing = 4;
    const auto trayIconCenter = calcTrayIconCenter();
    const auto taskbarRect = taskbarGeometry();
    const auto taskbarScreenEdge = taskbarOrientation();
    const auto screenRect = currentScreenRect();

    qCDebug(lcSystray) << "screenRect:" << screenRect;
    qCDebug(lcSystray) << "taskbarRect:" << taskbarRect;
    qCDebug(lcSystray) << "taskbarScreenEdge:" << taskbarScreenEdge;
    qCDebug(lcSystray) << "trayIconCenter:" << trayIconCenter;

    switch(taskbarScreenEdge) {
    case TaskBarPosition::Bottom:
        return {
            trayIconCenter.x(),
            screenRect.bottom() - taskbarRect.height() - spacing
        };
    case TaskBarPosition::Left:
        return {
            screenRect.left() + taskbarRect.width() + spacing,
            trayIconCenter.y()
        };
    case TaskBarPosition::Top:
        return {
            trayIconCenter.x(),
            screenRect.top() + taskbarRect.height() + spacing
        };
    case TaskBarPosition::Right:
        return {
            screenRect.right() - taskbarRect.width() - spacing,
            trayIconCenter.y()
        };
    }
    Q_UNREACHABLE();
}

QPoint Systray::computeWindowPosition(int width, int height) const
{
    const auto referencePoint = computeWindowReferencePoint();

    const auto taskbarScreenEdge = taskbarOrientation();
    const auto screenRect = currentScreenRect();

    const auto topLeft = [=]() {
        switch(taskbarScreenEdge) {
        case TaskBarPosition::Bottom:
            return referencePoint - QPoint(width / 2, height);
        case TaskBarPosition::Left:
            return referencePoint;
        case TaskBarPosition::Top:
            return referencePoint - QPoint(width / 2, 0);
        case TaskBarPosition::Right:
            return referencePoint - QPoint(width, 0);
        }
        Q_UNREACHABLE();
    }();
    const auto bottomRight = topLeft + QPoint(width, height);
    const auto windowRect = [=]() {
        const auto rect = QRect(topLeft, bottomRight);
        auto offset = QPoint();

        if (rect.left() < screenRect.left()) {
            offset.setX(screenRect.left() - rect.left() + 4);
        } else if (rect.right() > screenRect.right()) {
            offset.setX(screenRect.right() - rect.right() - 4);
        }

        if (rect.top() < screenRect.top()) {
            offset.setY(screenRect.top() - rect.top() + 4);
        } else if (rect.bottom() > screenRect.bottom()) {
            offset.setY(screenRect.bottom() - rect.bottom() - 4);
        }

        return rect.translated(offset);
    }();

    qCDebug(lcSystray) << "taskbarScreenEdge:" << taskbarScreenEdge;
    qCDebug(lcSystray) << "screenRect:" << screenRect;
    qCDebug(lcSystray) << "windowRect (reference)" << QRect(topLeft, bottomRight);
    qCDebug(lcSystray) << "windowRect (adjusted )" << windowRect;

    return windowRect.topLeft();
}

QPoint Systray::calcTrayIconCenter() const
{
    // QSystemTrayIcon::geometry() is broken for ages on most Linux DEs (invalid geometry returned)
    // thus we can use this only for Windows and macOS
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    auto trayIconCenter = geometry().center();
    return trayIconCenter;
#else
    // On Linux, fall back to mouse position (assuming tray icon is activated by mouse click)
    return QCursor::pos(currentScreen());
#endif
}

} // namespace OCC
