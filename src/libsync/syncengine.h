/*
 * Copyright (C) by Duncan Mac-Vicar P. <duncan@kde.org>
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

#ifndef CSYNCTHREAD_H
#define CSYNCTHREAD_H

#include <stdint.h>

#include <QMutex>
#include <QThread>
#include <QString>
#include <QSet>
#include <QMap>
#include <QStringList>
#include <QSharedPointer>
#include <set>

#include <csync.h>

// when do we go away with this private/public separation?
#include <csync_private.h>

#include "syncfileitem.h"
#include "progressdispatcher.h"
#include "common/utility.h"
#include "syncfilestatustracker.h"
#include "accountfwd.h"
#include "discoveryphase.h"
#include "common/checksums.h"

class QProcess;

namespace OCC {

class SyncJournalFileRecord;
class SyncJournalDb;
class OwncloudPropagator;

enum AnotherSyncNeeded {
    NoFollowUpSync,
    ImmediateFollowUp, // schedule this again immediately (limited amount of times)
    DelayedFollowUp // regularly schedule this folder again (around 1/minute, unlimited)
};

/**
 * @brief The SyncEngine class
 * @ingroup libsync
 */
class OWNCLOUDSYNC_EXPORT SyncEngine : public QObject
{
    Q_OBJECT
public:
    SyncEngine(AccountPtr account, const QString &localPath,
        const QString &remotePath, SyncJournalDb *journal);
    ~SyncEngine();

    static QString csyncErrorToString(CSYNC_STATUS);

    Q_INVOKABLE void startSync();
    void setNetworkLimits(int upload, int download);

    /* Abort the sync.  Called from the main thread */
    void abort();

    bool isSyncRunning() const { return _syncRunning; }

    void setSyncOptions(const SyncOptions &options) { _syncOptions = options; }
    bool ignoreHiddenFiles() const { return _csync_ctx->ignore_hidden_files; }
    void setIgnoreHiddenFiles(bool ignore) { _csync_ctx->ignore_hidden_files = ignore; }

    ExcludedFiles &excludedFiles() { return *_excludedFiles; }
    Utility::StopWatch &stopWatch() { return _stopWatch; }
    SyncFileStatusTracker &syncFileStatusTracker() { return *_syncFileStatusTracker; }

    /* Returns whether another sync is needed to complete the sync */
    AnotherSyncNeeded isAnotherSyncNeeded() { return _anotherSyncNeeded; }

    bool wasFileTouched(const QString &fn) const;

    AccountPtr account() const;
    SyncJournalDb *journal() const { return _journal; }
    QString localPath() const { return _localPath; }

    /**
     * Minimum age, in milisecond, of a file that can be uploaded.
     * Files more recent than that are not going to be uploaeded as they are considered
     * too young and possibly still changing
     */
    static qint64 minimumFileAgeForUpload; // in ms

    /**
     * Control whether local discovery should read from filesystem or db.
     *
     * If style is DatabaseAndFilesystem, paths a set of file paths relative to
     * the synced folder. All the parent directories of these paths will not
     * be read from the db and scanned on the filesystem.
     *
     * Note, the style and paths are only retained for the next sync and
     * revert afterwards. Use _lastLocalDiscoveryStyle to discover the last
     * sync's style.
     */
    void setLocalDiscoveryOptions(LocalDiscoveryStyle style, std::set<QByteArray> paths = {});

    /**
     * Returns whether the given folder-relative path should be locally discovered
     * given the local discovery options.
     *
     * Example: If path is 'foo/bar' and style is DatabaseAndFilesystem and dirs contains
     *     'foo/bar/touched_file', then the result will be true.
     */
    bool shouldDiscoverLocally(const QByteArray &path) const;

    /** Access the last sync run's local discovery style */
    LocalDiscoveryStyle lastLocalDiscoveryStyle() const { return _lastLocalDiscoveryStyle; }

signals:
    void csyncUnavailable();

    // During update, before reconcile
    void rootEtag(QString);

    // after the above signals. with the items that actually need propagating
    void aboutToPropagate(SyncFileItemVector &);

    // after each item completed by a job (successful or not)
    void itemCompleted(const SyncFileItemPtr &);

    void transmissionProgress(const ProgressInfo &progress);

    /// We've produced a new sync error of a type.
    void syncError(const QString &message, ErrorCategory category);

    void finished(bool success);
    void started();

    /**
     * Emited when the sync engine detects that all the files have been removed or change.
     * This usually happen when the server was reset or something.
     * Set *cancel to true in a slot connected from this signal to abort the sync.
     */
    void aboutToRemoveAllFiles(SyncFileItem::Direction direction, bool *cancel);
    /**
     * Emited when the sync engine detects that all the files are changed to dates in the past.
     * This usually happen when a backup was restored on the server from an earlier date.
     * Set *restore to true in a slot connected from this signal to re-upload all files.
     */
    void aboutToRestoreBackup(bool *restore);

    // A new file or folder was discovered and was not synced because of the confirmation feature
    void newBigObject(const QString &folder, bool isExternal, bool isFolder);

    /** Emitted when propagation has problems with a locked file.
     *
     * Forwarded from OwncloudPropagator::seenLockedFile.
     */
    void seenLockedFile(const QString &fileName);

private slots:
    void slotFolderDiscovered(bool local, const QString &folder);
    void slotRootEtagReceived(const QString &);

    /** Called when a SyncFileItem gets accepted for a sync.
     *
     * Mostly done in initial creation inside treewalkFile but
     * can also be called via the propagator for items that are
     * created during propagation.
     */
    void slotNewItem(const SyncFileItemPtr &item);

    void slotItemCompleted(const SyncFileItemPtr &item);
    void slotFinished(bool success);
    void slotProgress(const SyncFileItem &item, quint64 curent);
    void slotDiscoveryJobFinished(int updateResult);
    void slotCleanPollsJobAborted(const QString &error);

    /** Records that a file was touched by a job. */
    void slotAddTouchedFile(const QString &fn);

    /** Wipes the _touchedFiles hash */
    void slotClearTouchedFiles();

    /** Emit a summary error, unless it was seen before */
    void slotSummaryError(const QString &message);

    void slotInsufficientLocalStorage();
    void slotInsufficientRemoteStorage();

private:
    void handleSyncError(CSYNC *ctx, const char *state);
    void csyncError(const QString &message);

    QString journalDbFilePath() const;

    int treewalkFile(csync_file_stat_t *file, csync_file_stat_t *other, bool);
    bool checkErrorBlacklisting(SyncFileItem &item);

    // Cleans up unnecessary downloadinfo entries in the journal as well
    // as their temporary files.
    void deleteStaleDownloadInfos(const SyncFileItemVector &syncItems);

    // Removes stale uploadinfos from the journal.
    void deleteStaleUploadInfos(const SyncFileItemVector &syncItems);

    // Removes stale error blacklist entries from the journal.
    void deleteStaleErrorBlacklistEntries(const SyncFileItemVector &syncItems);

    // Removes stale and adds missing conflict records after sync
    void conflictRecordMaintenance();

    // cleanup and emit the finished signal
    void finalize(bool success);

    static bool s_anySyncRunning; //true when one sync is running somewhere (for debugging)

    // Must only be acessed during update and reconcile
    QMap<QString, SyncFileItemPtr> _syncItemMap;

    AccountPtr _account;
    QScopedPointer<CSYNC> _csync_ctx;
    bool _needsUpdate;
    bool _syncRunning;
    QString _localPath;
    QString _remotePath;
    QString _remoteRootEtag;
    SyncJournalDb *_journal;
    QPointer<DiscoveryMainThread> _discoveryMainThread;
    QSharedPointer<OwncloudPropagator> _propagator;

    // After a sync, only the syncdb entries whose filenames appear in this
    // set will be kept. See _temporarilyUnavailablePaths.
    QSet<QString> _seenFiles;

    // Some paths might be temporarily unavailable on the server, for
    // example due to 503 Storage not available. Deleting information
    // about the files from the database in these cases would lead to
    // incorrect synchronization.
    // Therefore all syncdb entries whose filename starts with one of
    // the paths in this set will be kept.
    // The specific case that fails otherwise is deleting a local file
    // while the remote says storage not available.
    QSet<QString> _temporarilyUnavailablePaths;

    QThread _thread;

    QScopedPointer<ProgressInfo> _progressInfo;

    QScopedPointer<ExcludedFiles> _excludedFiles;
    QScopedPointer<SyncFileStatusTracker> _syncFileStatusTracker;
    Utility::StopWatch _stopWatch;

    // maps the origin and the target of the folders that have been renamed
    QHash<QString, QString> _renamedFolders;
    QString adjustRenamedPath(const QString &original);

    /**
     * check if we are allowed to propagate everything, and if we are not, adjust the instructions
     * to recover
     */
    void checkForPermission(SyncFileItemVector &syncItems);
    RemotePermissions getPermissions(const QString &file) const;

    /**
     * Instead of downloading files from the server, upload the files to the server
     */
    void restoreOldFiles(SyncFileItemVector &syncItems);

    // true if there is at least one file which was not changed on the server
    bool _hasNoneFiles;

    // true if there is at leasr one file with instruction REMOVE
    bool _hasRemoveFile;

    // true if there is at least one file from the server that goes forward in time
    bool _hasForwardInTimeFiles;

    // number of files which goes back in time from the server
    int _backInTimeFiles;


    int _uploadLimit;
    int _downloadLimit;
    SyncOptions _syncOptions;

    /// Hook for computing checksums from csync_update
    CSyncChecksumHook _checksum_hook;

    AnotherSyncNeeded _anotherSyncNeeded;

    /** Stores the time since a job touched a file. */
    QMultiMap<QElapsedTimer, QString> _touchedFiles;

    /** For clearing the _touchedFiles variable after sync finished */
    QTimer _clearTouchedFilesTimer;

    /** List of unique errors that occurred in a sync run. */
    QSet<QString> _uniqueErrors;

    /** The kind of local discovery the last sync run used */
    LocalDiscoveryStyle _lastLocalDiscoveryStyle = LocalDiscoveryStyle::FilesystemOnly;
    LocalDiscoveryStyle _localDiscoveryStyle = LocalDiscoveryStyle::FilesystemOnly;
    std::set<QByteArray> _localDiscoveryPaths;
};
}

#endif // CSYNCTHREAD_H
