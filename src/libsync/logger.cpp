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

#include "logger.h"

#include "config.h"

#include <QDir>
#include <QStringList>
#include <QtGlobal>
#include <QTextCodec>
#include <qmetaobject.h>

#include <iostream>

#ifdef ZLIB_FOUND
#include <zlib.h>
#endif

#ifdef Q_OS_WIN
#include <io.h> // for stdout
#endif

namespace OCC {

QtMessageHandler s_originalMessageHandler = nullptr;

static void mirallLogCatcher(QtMsgType type, const QMessageLogContext &ctx, const QString &message)
{
    auto logger = Logger::instance();
    if (type == QtDebugMsg && !logger->logDebug()) {
        if (s_originalMessageHandler) {
            s_originalMessageHandler(type, ctx, message);
        }
    } else if (!logger->isNoop()) {
        logger->doLog(qFormatLogMessage(type, ctx, message));
    }
    if(type == QtCriticalMsg || type == QtFatalMsg) {
        std::cerr << qPrintable(qFormatLogMessage(type, ctx, message)) << std::endl;
    }

    if(type == QtFatalMsg) {
        if (!logger->isNoop()) {
            logger->close();
        }
#if defined(Q_OS_WIN)
    // Make application terminate in a way that can be caught by the crash reporter
        Utility::crash();
#endif
    }
}


Logger *Logger::instance()
{
    static Logger log;
    return &log;
}

Logger::Logger(QObject *parent)
    : QObject(parent)
{
    qSetMessagePattern("%{time yyyy-MM-dd hh:mm:ss:zzz} [ %{type} %{category} ]%{if-debug}\t[ %{function} ]%{endif}:\t%{message}");
#ifndef NO_MSG_HANDLER
   s_originalMessageHandler = qInstallMessageHandler(mirallLogCatcher);
#else
    Q_UNUSED(mirallLogCatcher)
#endif
}

Logger::~Logger()
{
#ifndef NO_MSG_HANDLER
    qInstallMessageHandler(nullptr);
#endif
}


void Logger::postGuiLog(const QString &title, const QString &message)
{
    emit guiLog(title, message);
}

void Logger::postOptionalGuiLog(const QString &title, const QString &message)
{
    emit optionalGuiLog(title, message);
}

void Logger::postGuiMessage(const QString &title, const QString &message)
{
    emit guiMessage(title, message);
}

void Logger::log(Log log)
{
    QString msg;
    if (_showTime) {
        msg = log.timeStamp.toString(QLatin1String("MM-dd hh:mm:ss:zzz")) + QLatin1Char(' ');
    }

    msg += log.message;
    // _logs.append(log);
    // std::cout << qPrintable(log.message) << std::endl;

    doLog(msg);
}

/**
 * Returns true if doLog does nothing and need not to be called
 */
bool Logger::isNoop() const
{
    QMutexLocker lock(&_mutex);
    return !_logstream;
}

bool Logger::isLoggingToFile() const
{
    QMutexLocker lock(&_mutex);
    return _logstream;
}

void Logger::doLog(const QString &msg)
{
    {
        QMutexLocker lock(&_mutex);
        if (_logstream) {
            (*_logstream) << msg << "\n";
            if (_doFileFlush)
                _logstream->flush();
        }
    }
    emit logWindowLog(msg);
}

void Logger::close()
{
    QMutexLocker lock(&_mutex);
    if (_logstream)
    {
        _logstream->flush();
        _logFile.close();
        _logstream.reset();
    }
}

void Logger::mirallLog(const QString &message)
{
    Log log_;
    log_.timeStamp = QDateTime::currentDateTimeUtc();
    log_.message = message;

    Logger::instance()->log(log_);
}

QString Logger::logFile() const
{
    return _logFile.fileName();
}

void Logger::setLogFile(const QString &name)
{
    QMutexLocker locker(&_mutex);
    if (_logstream) {
        _logstream.reset(nullptr);
        _logFile.close();
    }

    if (name.isEmpty()) {
        return;
    }

    bool openSucceeded = false;
    if (name == QLatin1String("-")) {
        openSucceeded = _logFile.open(stdout, QIODevice::WriteOnly);
    } else {
        _logFile.setFileName(name);
        openSucceeded = _logFile.open(QIODevice::WriteOnly);
    }

    if (!openSucceeded) {
        locker.unlock(); // Just in case postGuiMessage has a qDebug()
        postGuiMessage(tr("Error"),
            QString(tr("<nobr>File '%1'<br/>cannot be opened for writing.<br/><br/>"
                       "The log output can <b>not</b> be saved!</nobr>"))
                .arg(name));
        return;
    }

    _logstream.reset(new QTextStream(&_logFile));
    _logstream->setCodec(QTextCodec::codecForName("UTF-8"));
}

void Logger::setLogExpire(int expire)
{
    _logExpire = expire;
}

QString Logger::logDir() const
{
    return _logDirectory;
}

void Logger::setLogDir(const QString &dir)
{
    _logDirectory = dir;
}

void Logger::setLogFlush(bool flush)
{
    _doFileFlush = flush;
}

void Logger::setLogDebug(bool debug)
{
    QLoggingCategory::setFilterRules(debug ? QStringLiteral("nextcloud.*.debug=true") : QString());
    _logDebug = debug;
}

QString Logger::temporaryFolderLogDirPath() const
{
    QString dirName = APPLICATION_SHORTNAME + QString("-logdir");
    return QDir::temp().filePath(dirName);
}

void Logger::setupTemporaryFolderLogDir()
{
    auto dir = temporaryFolderLogDirPath();
    if (!QDir().mkpath(dir))
        return;
    setLogDebug(true);
    setLogExpire(4 /*hours*/);
    setLogDir(dir);
    _temporaryFolderLogDir = true;
}

void Logger::disableTemporaryFolderLogDir()
{
    if (!_temporaryFolderLogDir)
        return;

    enterNextLogFile();
    setLogDir(QString());
    setLogDebug(false);
    setLogFile(QString());
    _temporaryFolderLogDir = false;
}

static bool compressLog(const QString &originalName, const QString &targetName)
{
#ifdef ZLIB_FOUND
    QFile original(originalName);
    if (!original.open(QIODevice::ReadOnly))
        return false;
    auto compressed = gzopen(targetName.toUtf8(), "wb");
    if (!compressed) {
        return false;
    }

    while (!original.atEnd()) {
        auto data = original.read(1024 * 1024);
        auto written = gzwrite(compressed, data.data(), data.size());
        if (written != data.size()) {
            gzclose(compressed);
            return false;
        }
    }
    gzclose(compressed);
    return true;
#else
    return false;
#endif
}

void Logger::enterNextLogFile()
{
    if (!_logDirectory.isEmpty()) {

        QDir dir(_logDirectory);
        if (!dir.exists()) {
            dir.mkpath(".");
        }

        // Tentative new log name, will be adjusted if one like this already exists
        QDateTime now = QDateTime::currentDateTime();
        QString newLogName = now.toString("yyyyMMdd_HHmm") + "_owncloud.log";

        // Expire old log files and deal with conflicts
        QStringList files = dir.entryList(QStringList("*owncloud.log.*"),
            QDir::Files, QDir::Name);
        QRegExp rx(R"(.*owncloud\.log\.(\d+).*)");
        int maxNumber = -1;
        foreach (const QString &s, files) {
            if (_logExpire > 0) {
                QFileInfo fileInfo(dir.absoluteFilePath(s));
                if (fileInfo.lastModified().addSecs(60 * 60 * _logExpire) < now) {
                    dir.remove(s);
                }
            }
            if (s.startsWith(newLogName) && rx.exactMatch(s)) {
                maxNumber = qMax(maxNumber, rx.cap(1).toInt());
            }
        }
        newLogName.append("." + QString::number(maxNumber + 1));

        auto previousLog = _logFile.fileName();
        setLogFile(dir.filePath(newLogName));

        // Compress the previous log file. On a restart this can be the most recent
        // log file.
        auto logToCompress = previousLog;
        if (logToCompress.isEmpty() && files.size() > 0 && !files.last().endsWith(".gz"))
            logToCompress = dir.absoluteFilePath(files.last());
        if (!logToCompress.isEmpty()) {
            QString compressedName = logToCompress + ".gz";
            if (compressLog(logToCompress, compressedName)) {
                QFile::remove(logToCompress);
            } else {
                QFile::remove(compressedName);
            }
        }
    }
}

} // namespace OCC
