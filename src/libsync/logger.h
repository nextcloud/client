/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#ifndef LOGGER_H
#define LOGGER_H

#include <QObject>
#include <QList>
#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <qmutex.h>

#include "common/utility.h"
#include "owncloudlib.h"

namespace OCC {

struct Log
{
    QDateTime timeStamp;
    QString message;
};

/**
 * @brief The Logger class
 * @ingroup libsync
 */
class OWNCLOUDSYNC_EXPORT Logger : public QObject
{
    Q_OBJECT
public:
    bool isNoop() const;
    bool isLoggingToFile() const;

    void log(Log log);
    void doLog(const QString &log);
    void close();

    static void mirallLog(const QString &message);

    const QList<Log> &logs() const { return _logs; }

    static Logger *instance();

    void postGuiLog(const QString &title, const QString &message);
    void postOptionalGuiLog(const QString &title, const QString &message);
    void postGuiMessage(const QString &title, const QString &message);

    QString logFile() const;
    void setLogFile(const QString &name);

    void setLogExpire(int expire);

    QString logDir() const;
    void setLogDir(const QString &dir);

    void setLogFlush(bool flush);

    bool logDebug() const { return _logDebug; }
    void setLogDebug(bool debug);

    /** Returns where the automatic logdir would be */
    QString temporaryFolderLogDirPath() const;

    /** Sets up default dir log setup.
     *
     * logdir: a temporary folder
     * logexpire: 4 hours
     * logdebug: true
     *
     * Used in conjunction with ConfigFile::automaticLogDir
     */
    void setupTemporaryFolderLogDir();

    /** For switching off via logwindow */
    void disableTemporaryFolderLogDir();

    void addLogRule(const QSet<QString> &rules) {
        setLogRules(_logRules + rules);
    }
    void removeLogRule(const QSet<QString> &rules) {
        setLogRules(_logRules - rules);
    }
    void setLogRules(const QSet<QString> &rules);

signals:
    void logWindowLog(const QString &);

    void guiLog(const QString &, const QString &);
    void guiMessage(const QString &, const QString &);
    void optionalGuiLog(const QString &, const QString &);

public slots:
    void enterNextLogFile();

private:
    Logger(QObject *parent = nullptr);
    ~Logger();
    QList<Log> _logs;
    bool _showTime = true;
    QFile _logFile;
    bool _doFileFlush = false;
    int _logExpire = 0;
    bool _logDebug = false;
    QScopedPointer<QTextStream> _logstream;
    mutable QMutex _mutex;
    QString _logDirectory;
    bool _temporaryFolderLogDir = false;
    QSet<QString> _logRules;
};

} // namespace OCC

#endif // LOGGER_H
