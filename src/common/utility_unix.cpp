/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <QStandardPaths>
#include <QtGlobal>

namespace OCC {

static void setupFavLink_private(const QString &folder)
{
    // Nautilus: add to ~/.gtk-bookmarks
    QFile gtkBookmarks(QDir::homePath() + QLatin1String("/.config/gtk-3.0/bookmarks"));
    QByteArray folderUrl = "file://" + folder.toUtf8();
    if (gtkBookmarks.open(QFile::ReadWrite)) {
        QByteArray places = gtkBookmarks.readAll();
        if (!places.contains(folderUrl)) {
            places += folderUrl;
            gtkBookmarks.reset();
            gtkBookmarks.write(places + '\n');
        }
    }
}

// returns the autostart directory the linux way
// and respects the XDG_CONFIG_HOME env variable
QString getUserAutostartDir_private()
{
    QString config = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    config += QLatin1String("/autostart/");
    return config;
}

bool hasLaunchOnStartup_private(const QString &appName)
{
    Q_UNUSED(appName)
    QString desktopFileLocation = getUserAutostartDir_private()
                                    + QLatin1String(LINUX_APPLICATION_ID)
                                    + QLatin1String(".desktop");
    return QFile::exists(desktopFileLocation);
}

void setLaunchOnStartup_private(const QString &appName, const QString &guiName, bool enable)
{
    Q_UNUSED(appName)
    QString userAutoStartPath = getUserAutostartDir_private();
    QString desktopFileLocation = userAutoStartPath
                                    + QLatin1String(LINUX_APPLICATION_ID)
                                    + QLatin1String(".desktop");
    if (enable) {
        if (!QDir().exists(userAutoStartPath) && !QDir().mkpath(userAutoStartPath)) {
            qCWarning(lcUtility) << "Could not create autostart folder" << userAutoStartPath;
            return;
        }
        QFile iniFile(desktopFileLocation);
        if (!iniFile.open(QIODevice::WriteOnly)) {
            qCWarning(lcUtility) << "Could not write auto start entry" << desktopFileLocation;
            return;
        }
        // When running inside an AppImage, we need to set the path to the
        // AppImage instead of the path to the executable
        const QString appImagePath = qEnvironmentVariable("APPIMAGE");
        const bool runningInsideAppImage = !appImagePath.isNull() && QFile::exists(appImagePath);
        const QString executablePath = runningInsideAppImage ? appImagePath : QCoreApplication::applicationFilePath();

        QTextStream ts(&iniFile);
        ts.setCodec("UTF-8");
        ts << QLatin1String("[Desktop Entry]"); ts.flush();
        ts << QLatin1String("Name=") << guiName; ts.flush();
        ts << QLatin1String("GenericName=") << QLatin1String("File Synchronizer"); ts.flush();
        ts << QLatin1String("Exec=\"") << executablePath << "\" --background"; ts.flush();
        ts << QLatin1String("Terminal=") << "false"; ts.flush();
        ts << QLatin1String("Icon=") << APPLICATION_ICON_NAME; ts.flush();
        ts << QLatin1String("Categories=") << QLatin1String("Network"); ts.flush();
        ts << QLatin1String("Type=") << QLatin1String("Application"); ts.flush();
        ts << QLatin1String("StartupNotify=") << "false"; ts.flush();
        ts << QLatin1String("X-GNOME-Autostart-enabled=") << "true"; ts.flush();
        ts << QLatin1String("X-GNOME-Autostart-Delay=10"); ts.flush();
    } else {
        if (!QFile::remove(desktopFileLocation)) {
            qCWarning(lcUtility) << "Could not remove autostart desktop file";
        }
    }
}

static inline bool hasDarkSystray_private()
{
    return true;
}

} // namespace OCC
