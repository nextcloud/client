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

#ifndef UTILITY_H
#define UTILITY_H


#include "ocsynclib.h"
#include <QString>
#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QLoggingCategory>
#include <QMap>
#include <QUrl>
#include <QUrlQuery>
#include <functional>
#include <memory>

#ifdef Q_OS_WIN
#include <QRect>
#include <windows.h>
#endif

class QSettings;

namespace OCC {

class SyncJournal;

Q_DECLARE_LOGGING_CATEGORY(lcUtility)

/** \addtogroup libsync
 *  @{
 */
namespace Utility {
    OCSYNC_EXPORT void sleep(int sec);
    OCSYNC_EXPORT void usleep(int usec);
    OCSYNC_EXPORT QString formatFingerprint(const QByteArray &, bool colonSeparated = true);
    OCSYNC_EXPORT void setupFavLink(const QString &folder);
    OCSYNC_EXPORT bool writeRandomFile(const QString &fname, int size = -1);
    OCSYNC_EXPORT QString octetsToString(qint64 octets);
    OCSYNC_EXPORT QByteArray userAgentString();
    OCSYNC_EXPORT QByteArray friendlyUserAgentString();
    /**
      * @brief Return whether launch on startup is enabled system wide.
      *
      * If this returns true, the checkbox for user specific launch
      * on startup will be hidden.
      *
      * Currently only implemented on Windows.
      */
    OCSYNC_EXPORT bool hasSystemLaunchOnStartup(const QString &appName);
    OCSYNC_EXPORT bool hasLaunchOnStartup(const QString &appName);
    OCSYNC_EXPORT void setLaunchOnStartup(const QString &appName, const QString &guiName, bool launch);
    OCSYNC_EXPORT uint convertSizeToUint(size_t &convertVar);
    OCSYNC_EXPORT int convertSizeToInt(size_t &convertVar);

#ifdef Q_OS_WIN
    OCSYNC_EXPORT DWORD convertSizeToDWORD(size_t &convertVar);
#endif

    /**
     * Return the amount of free space available.
     *
     * \a path must point to a directory
     */
    OCSYNC_EXPORT qint64 freeDiskSpace(const QString &path);

    /**
     * @brief compactFormatDouble - formats a double value human readable.
     *
     * @param value the value to format.
     * @param prec the precision.
     * @param unit an optional unit that is appended if present.
     * @return the formatted string.
     */
    OCSYNC_EXPORT QString compactFormatDouble(double value, int prec, const QString &unit = QString());

    // porting methods
    OCSYNC_EXPORT QString escape(const QString &);

    // conversion function QDateTime <-> time_t   (because the ones builtin work on only unsigned 32bit)
    OCSYNC_EXPORT QDateTime qDateTimeFromTime_t(qint64 t);
    OCSYNC_EXPORT qint64 qDateTimeToTime_t(const QDateTime &t);

    /**
     * @brief Convert milliseconds duration to human readable string.
     * @param quint64 msecs the milliseconds to convert to string.
     * @return an HMS representation of the milliseconds value.
     *
     * durationToDescriptiveString1 describes the duration in a single
     * unit, like "5 minutes" or "2 days".
     *
     * durationToDescriptiveString2 uses two units where possible, so
     * "5 minutes 43 seconds" or "1 month 3 days".
     */
    OCSYNC_EXPORT QString durationToDescriptiveString1(quint64 msecs);
    OCSYNC_EXPORT QString durationToDescriptiveString2(quint64 msecs);

    /**
     * @brief hasDarkSystray - determines whether the systray is dark or light.
     *
     * Use this to check if the OS has a dark or a light systray.
     *
     * The value might change during the execution of the program
     * (e.g. on OS X 10.10).
     *
     * @return bool which is true for systems with dark systray.
     */
    OCSYNC_EXPORT bool hasDarkSystray();

    // convenience OS detection methods
    inline bool isWindows();
    inline bool isMac();
    inline bool isUnix();
    inline bool isLinux(); // use with care
    inline bool isBSD(); // use with care, does not match OS X

    // convenience variable-safe QT_DEBUG passthrough
    inline bool isDebugBuild();

    OCSYNC_EXPORT QString platformName();
    // crash helper for --debug
    OCSYNC_EXPORT void crash();

    // Case preserving file system underneath?
    // if this function returns true, the file system is case preserving,
    // that means "test" means the same as "TEST" for filenames.
    // if false, the two cases are two different files.
    OCSYNC_EXPORT bool fsCasePreserving();

    // Check if two pathes that MUST exist are equal. This function
    // uses QDir::canonicalPath() to judge and cares for the systems
    // case sensitivity.
    OCSYNC_EXPORT bool fileNamesEqual(const QString &fn1, const QString &fn2);

    // Call the given command with the switch --version and rerun the first line
    // of the output.
    // If command is empty, the function calls the running application which, on
    // Linux, might have changed while this one is running.
    // For Mac and Windows, it returns QString()
    OCSYNC_EXPORT QByteArray versionOfInstalledBinary(const QString &command = QString());

    OCSYNC_EXPORT QString fileNameForGuiUse(const QString &fName);

    OCSYNC_EXPORT QByteArray normalizeEtag(QByteArray etag);

    /**
     * @brief timeAgoInWords - human readable time span
     *
     * Use this to get a string that describes the timespan between the first and
     * the second timestamp in a human readable and understandable form.
     *
     * If the second parameter is ommitted, the current time is used.
     */
    OCSYNC_EXPORT QString timeAgoInWords(const QDateTime &dt, const QDateTime &from = QDateTime());

    class OCSYNC_EXPORT StopWatch
    {
    private:
        QMap<QString, quint64> _lapTimes;
        QDateTime _startTime;
        QElapsedTimer _timer;

    public:
        void start();
        quint64 stop();
        quint64 addLapTime(const QString &lapName);
        void reset();

        // out helpers, return the measured times.
        QDateTime startTime() const;
        QDateTime timeOfLap(const QString &lapName) const;
        quint64 durationOfLap(const QString &lapName) const;
    };

    /**
     * @brief Sort a QStringList in a way that's appropriate for filenames
     */
    OCSYNC_EXPORT void sortFilenames(QStringList &fileNames);

    /** Appends concatPath and queryItems to the url */
    OCSYNC_EXPORT QUrl concatUrlPath(
        const QUrl &url, const QString &concatPath,
        const QUrlQuery &queryItems = {});

    /**  Returns a new settings pre-set in a specific group.  The Settings will be created
         with the given parent. If no parent is specified, the caller must destroy the settings */
    OCSYNC_EXPORT std::unique_ptr<QSettings> settingsWithGroup(const QString &group, QObject *parent = nullptr);

    /** Sanitizes a string that shall become part of a filename.
     *
     * Filters out reserved characters like
     * - unicode control and format characters
     * - reserved characters: /, ?, <, >, \, :, *, |, and "
     *
     * Warning: This does not sanitize the whole resulting string, so
     * - unix reserved filenames ('.', '..')
     * - trailing periods and spaces
     * - windows reserved filenames ('CON' etc)
     * will pass unchanged.
     */
    OCSYNC_EXPORT QString sanitizeForFileName(const QString &name);

    /** Returns a file name based on \a fn that's suitable for a conflict.
     */
    OCSYNC_EXPORT QString makeConflictFileName(
        const QString &fn, const QDateTime &dt, const QString &user);

    /** Returns whether a file name indicates a conflict file
     */
    OCSYNC_EXPORT bool isConflictFile(const char *name);
    OCSYNC_EXPORT bool isConflictFile(const QString &name);

    /** Find the base name for a conflict file name, using name pattern only
     *
     * Will return an empty string if it's not a conflict file.
     *
     * Prefer to use the data from the conflicts table in the journal to determine
     * a conflict's base file, see SyncJournal::conflictFileBaseName()
     */
    OCSYNC_EXPORT QByteArray conflictFileBaseNameFromPattern(const QByteArray &conflictName);

#ifdef Q_OS_WIN
    OCSYNC_EXPORT bool registryKeyExists(HKEY hRootKey, const QString &subKey);
    OCSYNC_EXPORT QVariant registryGetKeyValue(HKEY hRootKey, const QString &subKey, const QString &valueName);
    OCSYNC_EXPORT bool registrySetKeyValue(HKEY hRootKey, const QString &subKey, const QString &valueName, DWORD type, const QVariant &value);
    OCSYNC_EXPORT bool registryDeleteKeyTree(HKEY hRootKey, const QString &subKey);
    OCSYNC_EXPORT bool registryDeleteKeyValue(HKEY hRootKey, const QString &subKey, const QString &valueName);
    OCSYNC_EXPORT bool registryWalkSubKeys(HKEY hRootKey, const QString &subKey, const std::function<void(HKEY, const QString &)> &callback);
    OCSYNC_EXPORT QRect getTaskbarDimensions();

    // Possibly refactor to share code with UnixTimevalToFileTime in c_time.c
    OCSYNC_EXPORT void UnixTimeToFiletime(time_t t, FILETIME *filetime);
    OCSYNC_EXPORT void FiletimeToLargeIntegerFiletime(FILETIME *filetime, LARGE_INTEGER *hundredNSecs);
    OCSYNC_EXPORT void UnixTimeToLargeIntegerFiletime(time_t t, LARGE_INTEGER *hundredNSecs);

    OCSYNC_EXPORT QString formatWinError(long error);

    class OCSYNC_EXPORT NtfsPermissionLookupRAII
    {
    public:
        /**
         * NTFS permissions lookup is diabled by default for performance reasons
         * Enable it and disable it again once we leave the scope
         * https://doc.qt.io/Qt-5/qfileinfo.html#ntfs-permissions
         */
        NtfsPermissionLookupRAII();
        ~NtfsPermissionLookupRAII();

    private:
        Q_DISABLE_COPY(NtfsPermissionLookupRAII);
    };

#endif
}
/** @} */ // \addtogroup

inline bool Utility::isWindows()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

inline bool Utility::isMac()
{
#ifdef Q_OS_MAC
    return true;
#else
    return false;
#endif
}

inline bool Utility::isUnix()
{
#ifdef Q_OS_UNIX
    return true;
#else
    return false;
#endif
}

inline bool Utility::isLinux()
{
#if defined(Q_OS_LINUX)
    return true;
#else
    return false;
#endif
}

inline bool Utility::isBSD()
{
#if defined(Q_OS_FREEBSD) || defined(Q_OS_NETBSD) || defined(Q_OS_OPENBSD)
    return true;
#else
    return false;
#endif
}


inline bool Utility::isDebugBuild()
{
#ifdef QT_DEBUG
    return true;
#else
    return false;
#endif
}

}
#endif // UTILITY_H
