/*
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
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

#pragma once

#include "owncloudlib.h"
#include <QString>
#include <QSharedPointer>
#include <chrono>
#include "common/vfs.h"

namespace OCC {

/**
 * Value class containing the options given to the sync engine
 */
struct OWNCLOUDSYNC_EXPORT SyncOptions
{
    SyncOptions()
        : _vfs(new VfsOff)
    {}

    /** Maximum size (in Bytes) a folder can have without asking for confirmation.
     * -1 means infinite */
    qint64 _newBigFolderSizeLimit = -1;

    /** If a confirmation should be asked for external storages */
    bool _confirmExternalStorage = false;

    /** If remotely deleted files are needed to move to trash */
    bool _moveFilesToTrash = false;

    /** Create a virtual file for new files instead of downloading. May not be null */
    QSharedPointer<Vfs> _vfs;

    /** The initial un-adjusted chunk size in bytes for chunked uploads, both
     * for old and new chunking algorithm, which classifies the item to be chunked
     *
     * In chunkingNG, when dynamic chunk size adjustments are done, this is the
     * starting value and is then gradually adjusted within the
     * minChunkSize / maxChunkSize bounds.
     */
    qint64 _initialChunkSize = 10 * 1000 * 1000; // 10MB

    /** The minimum chunk size in bytes for chunked uploads */
    qint64 _minChunkSize = 1 * 1000 * 1000; // 1MB

    /** The maximum chunk size in bytes for chunked uploads */
    qint64 _maxChunkSize = 1000 * 1000 * 1000; // 1000MB

    /** The target duration of chunk uploads for dynamic chunk sizing.
     *
     * Set to 0 it will disable dynamic chunk sizing.
     */
    std::chrono::milliseconds _targetChunkUploadDuration = std::chrono::minutes(1);

    /** The maximum number of active jobs in parallel  */
    int _parallelNetworkJobs = 6;
};


}
