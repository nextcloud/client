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

#include <QRegExp>

#include "syncrunfilelog.h"
#include "common/utility.h"
#include "filesystem.h"
#include <qfileinfo.h>

namespace OCC {

SyncRunFileLog::SyncRunFileLog() = default;

QString SyncRunFileLog::dateTimeStr(const QDateTime &dt)
{
    return dt.toString(Qt::ISODate);
}

void SyncRunFileLog::start(const QString &folderPath)
{
    const qint64 logfileMaxSize = 10 * 1024 * 1024; // 10MiB

    const QString logpath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if(!QDir(logpath).exists()) {
        QDir().mkdir(logpath);
    }

    int length = folderPath.split(QLatin1String("/")).length();
    QString filenameSingle = folderPath.split(QLatin1String("/")).at(length - 2);
    QString filename = logpath + QLatin1String("/") + filenameSingle + QLatin1String("_sync.log");

    int depthIndex = 2;
    while(QFile::exists(filename)) {

        QFile file(filename);
        file.open(QIODevice::ReadOnly| QIODevice::Text);
        QTextStream in(&file);
        QString line = in.readLine();

        if(QString::compare(folderPath,line,Qt::CaseSensitive)!=0) {
            depthIndex++;
            if(depthIndex <= length) {
                filenameSingle = folderPath.split(QLatin1String("/")).at(length - depthIndex) + QString("_") ///
                        + filenameSingle;
                filename = logpath+ QLatin1String("/") + filenameSingle + QLatin1String("_sync.log");
            }
            else {
                filenameSingle = filenameSingle + QLatin1String("_1");
                filename = logpath + QLatin1String("/") + filenameSingle + QLatin1String("_sync.log");
            }
        }
        else break;
    }

    // When the file is too big, just rename it to an old name.
    QFileInfo info(filename);
    bool exists = info.exists();
    if (exists && info.size() > logfileMaxSize) {
        exists = false;
        QString newFilename = filename + QLatin1String(".1");
        QFile::remove(newFilename);
        QFile::rename(filename, newFilename);
    }
    _file.reset(new QFile(filename));

    _file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    _out.setDevice(_file.data());


    if (!exists) {
        _out << folderPath; _out.flush();
        // We are creating a new file, add the note.
        _out << "# timestamp | duration | file | instruction | dir | modtime | etag | "
                "size | fileId | status | errorString | http result code | "
                "other size | other modtime | X-Request-ID";
        _out.flush();

        FileSystem::setFileHidden(filename, true);
    }


    _totalDuration.start();
    _lapDuration.start();
    _out << "#=#=#=# Syncrun started " << dateTimeStr(QDateTime::currentDateTimeUtc()); _out.flush();
}
void SyncRunFileLog::logItem(const SyncFileItem &item)
{
    // don't log the directory items that are in the list
    if (item._direction == SyncFileItem::None
        || item._instruction == CSYNC_INSTRUCTION_IGNORE) {
        return;
    }
    QString ts = QString::fromLatin1(item._responseTimeStamp);
    if (ts.length() > 6) {
        QRegExp rx(R"((\d\d:\d\d:\d\d))");
        if (ts.contains(rx)) {
            ts = rx.cap(0);
        }
    }

    const QChar L = QLatin1Char('|');
    _out << ts << L;
    _out << L;
    if (item._instruction != CSYNC_INSTRUCTION_RENAME) {
        _out << item.destination() << L;
    } else {
        _out << item._file << QLatin1String(" -> ") << item._renameTarget << L;
    }
    _out << item._instruction << L;
    _out << item._direction << L;
    _out << QString::number(item._modtime) << L;
    _out << item._etag << L;
    _out << QString::number(item._size) << L;
    _out << item._fileId << L;
    _out << item._status << L;
    _out << item._errorString << L;
    _out << QString::number(item._httpErrorCode) << L;
    _out << QString::number(item._previousSize) << L;
    _out << QString::number(item._previousModtime) << L;
    _out << item._requestId << L;

    _out.flush();
}

void SyncRunFileLog::logLap(const QString &name)
{
    _out << "#=#=#=#=# " << name << " " << dateTimeStr(QDateTime::currentDateTimeUtc())
         << " (last step: " << _lapDuration.restart() << " msec"
         << ", total: " << _totalDuration.elapsed() << " msec)"; _out.flush();
}

void SyncRunFileLog::finish()
{
    _out << "#=#=#=# Syncrun finished " << dateTimeStr(QDateTime::currentDateTimeUtc())
         << " (last step: " << _lapDuration.elapsed() << " msec"
         << ", total: " << _totalDuration.elapsed() << " msec)"; _out.flush();
    _file->close();
}
}
