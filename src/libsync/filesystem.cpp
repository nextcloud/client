/*
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
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

#include "filesystem.h"

#include "common/utility.h"
#include <QFile>
#include <QFileInfo>
#include <sys/time.h>

// We use some internals of csync:
extern "C" int c_utimes(const char *, const struct timeval *);

#include "csync.h"
#include "vio/csync_vio_local.h"
#include "std/c_string.h"
#include "std/c_utf8.h"

namespace OCC {

bool FileSystem::fileEquals(const QString &fn1, const QString &fn2)
{
    // compare two files with given filename and return true if they have the same content
    QFile f1(fn1);
    QFile f2(fn2);
    if (!f1.open(QIODevice::ReadOnly) || !f2.open(QIODevice::ReadOnly)) {
        qCWarning(lcFileSystem) << "fileEquals: Failed to open " << fn1 << "or" << fn2;
        return false;
    }

    if (getSize(fn1) != getSize(fn2)) {
        return false;
    }

    const int BufferSize = 16 * 1024;
    char buffer1[BufferSize];
    char buffer2[BufferSize];
    do {
        int r = f1.read(buffer1, BufferSize);
        if (f2.read(buffer2, BufferSize) != r) {
            // this should normally not happen: the files are supposed to have the same size.
            return false;
        }
        if (r <= 0) {
            return true;
        }
        if (memcmp(buffer1, buffer2, r) != 0) {
            return false;
        }
    } while (true);
    return false;
}

time_t FileSystem::getModTime(const QString &filename)
{
    csync_file_stat_t stat;
    qint64 result = -1;
    if (csync_vio_local_stat(filename.toUtf8().data(), &stat) != -1
        && (stat.modtime != 0)) {
        result = stat.modtime;
    } else {
        qCWarning(lcFileSystem) << "Could not get modification time for" << filename
                                << "with csync, using QFileInfo";
        result = Utility::qDateTimeToTime_t(QFileInfo(filename).lastModified());
    }
    return result;
}

bool FileSystem::setModTime(const QString &filename, time_t modTime)
{
    struct timeval times[2];
    times[0].tv_sec = times[1].tv_sec = modTime;
    times[0].tv_usec = times[1].tv_usec = 0;
    int rc = c_utimes(filename.toUtf8().data(), times);
    if (rc != 0) {
        qCWarning(lcFileSystem) << "Error setting mtime for" << filename
                                << "failed: rc" << rc << ", errno:" << errno;
        return false;
    }
    return true;
}

bool FileSystem::fileChanged(const QString &fileName,
    qint64 previousSize,
    time_t previousMtime)
{
    return getSize(fileName) != previousSize
        || getModTime(fileName) != previousMtime;
}

bool FileSystem::verifyFileUnchanged(const QString &fileName,
    qint64 previousSize,
    time_t previousMtime)
{
    const qint64 actualSize = getSize(fileName);
    const time_t actualMtime = getModTime(fileName);
    if (actualSize != previousSize || actualMtime != previousMtime) {
        qCInfo(lcFileSystem) << "File" << fileName << "has changed:"
                             << "size: " << previousSize << "<->" << actualSize
                             << ", mtime: " << previousMtime << "<->" << actualMtime;
        return false;
    }
    return true;
}

#ifdef Q_OS_WIN
static qint64 getSizeWithCsync(const QString &filename)
{
    qint64 result = 0;
    csync_file_stat_t stat;
    if (csync_vio_local_stat(filename.toUtf8().data(), &stat) != -1) {
        result = stat.size;
    } else {
        qCWarning(lcFileSystem) << "Could not get size for" << filename << "with csync";
    }
    return result;
}
#endif

qint64 FileSystem::getSize(const QString &filename)
{
#ifdef Q_OS_WIN
    if (isLnkFile(filename)) {
        // Use csync to get the file size. Qt seems unable to get at it.
        return getSizeWithCsync(filename);
    }
#endif
    return QFileInfo(filename).size();
}


} // namespace OCC
