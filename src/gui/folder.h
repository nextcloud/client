/*
 * Copyright (C) by Duncan Mac-Vicar P. <duncan@kde.org>
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
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

#ifndef MIRALL_FOLDER_H
#define MIRALL_FOLDER_H

#include "syncresult.h"
#include "progressdispatcher.h"
#include "common/syncjournaldb.h"
#include "networkjobs.h"

#include <csync.h>

#include <QObject>
#include <QStringList>
#include <QUuid>
#include <set>
#include <chrono>

class QThread;
class QSettings;

namespace OCC {

class SyncEngine;
class AccountState;
class SyncRunFileLog;
class FolderWatcher;

/**
 * @brief The FolderDefinition class
 * @ingroup gui
 */
class FolderDefinition
{
public:
    FolderDefinition()
        : paused(false)
        , ignoreHiddenFiles(false)
    {
    }

    /// The name of the folder in the ui and internally
    QString alias;
    /// path on local machine
    QString localPath;
    /// path to the journal, usually in QStandardPaths::AppDataLocation
    QString journalPath;
    /// path on remote
    QString targetPath;
    /// whether the folder is paused
    bool paused;
    /// whether the folder syncs hidden files
    bool ignoreHiddenFiles;
    /// the folder has client side encryption
    bool isClientSideEncrypted;
    /// The CLSID where this folder appears in registry for the Explorer navigation pane entry.
    QUuid navigationPaneClsid;

    /// Saves the folder definition, creating a new settings group.
    static void save(QSettings &settings, const FolderDefinition &folder);

    /// Reads a folder definition from a settings group with the name 'alias'.
    static bool load(QSettings &settings, const QString &alias,
        FolderDefinition *folder);

    /// Ensure / as separator and trailing /.
    static QString prepareLocalPath(const QString &path);

    /// Ensure starting / and no ending /.
    static QString prepareTargetPath(const QString &path);

    /// journalPath relative to localPath.
    QString absoluteJournalPath() const;

    /// Returns the relative journal path that's appropriate for this folder and account.
    QString defaultJournalPath(AccountPtr account);
};

/**
 * @brief The Folder class
 * @ingroup gui
 */
class Folder : public QObject
{
    Q_OBJECT

public:
    Folder(const FolderDefinition &definition, AccountState *accountState, QObject *parent = nullptr);

    ~Folder();

    typedef QMap<QString, Folder *> Map;
    typedef QMapIterator<QString, Folder *> MapIterator;

    /**
     * The account the folder is configured on.
     */
    AccountState *accountState() const { return _accountState.data(); }

    /**
     * alias or nickname
     */
    QString alias() const;
    QString shortGuiRemotePathOrAppName() const; // since 2.0 we don't want to show aliases anymore, show the path instead

    /**
     * short local path to display on the GUI  (native separators)
     */
    QString shortGuiLocalPath() const;

    /**
     * canonical local folder path, always ends with /
     */
    QString path() const;

    /**
     * cleaned canonical folder path, like path() but never ends with a /
     *
     * Wrapper for QDir::cleanPath(path()) except for "Z:/",
     * where it returns "Z:" instead of "Z:/".
     */
    QString cleanPath() const;

    /**
     * remote folder path
     */
    QString remotePath() const;

    void setNavigationPaneClsid(const QUuid &clsid) { _definition.navigationPaneClsid = clsid; }
    QUuid navigationPaneClsid() const { return _definition.navigationPaneClsid; }

    /**
     * remote folder path with server url
     */
    QUrl remoteUrl() const;

    /**
     * switch sync on or off
     */
    void setSyncPaused(bool);

    bool syncPaused() const;

    /**
     * Returns true when the folder may sync.
     */
    bool canSync() const;

    void prepareToSync();

    /**
     * True if the folder is busy and can't initiate
     * a synchronization
     */
    virtual bool isBusy() const;

    /**
     * return the last sync result with error message and status
     */
    SyncResult syncResult() const;

    /**
      * This is called if the sync folder definition is removed. Do cleanups here.
      */
    virtual void wipe();

    void setSyncState(SyncResult::Status state);

    void setDirtyNetworkLimits();

    /**
      * Ignore syncing of hidden files or not. This is defined in the
      * folder definition
      */
    bool ignoreHiddenFiles();
    void setIgnoreHiddenFiles(bool ignore);

    // Used by the Socket API
    SyncJournalDb *journalDb() { return &_journal; }
    SyncEngine &syncEngine() { return *_engine; }

    RequestEtagJob *etagJob() { return _requestEtagJob; }
    std::chrono::milliseconds msecSinceLastSync() const { return std::chrono::milliseconds(_timeSinceLastSyncDone.elapsed()); }
    std::chrono::milliseconds msecLastSyncDuration() const { return _lastSyncDuration; }
    int consecutiveFollowUpSyncs() const { return _consecutiveFollowUpSyncs; }
    int consecutiveFailingSyncs() const { return _consecutiveFailingSyncs; }

    /// Saves the folder data in the account's settings.
    void saveToSettings() const;
    /// Removes the folder from the account's settings.
    void removeFromSettings() const;

    /**
      * Returns whether a file inside this folder should be excluded.
      */
    bool isFileExcludedAbsolute(const QString &fullPath) const;

    /**
      * Returns whether a file inside this folder should be excluded.
      */
    bool isFileExcludedRelative(const QString &relativePath) const;

    /** Calls schedules this folder on the FolderMan after a short delay.
      *
      * This should be used in situations where a sync should be triggered
      * because a local file was modified. Syncs don't upload files that were
      * modified too recently, and this delay ensures the modification is
      * far enough in the past.
      *
      * The delay doesn't reset with subsequent calls.
      */
    void scheduleThisFolderSoon();

    /**
      * Migration: When this flag is true, this folder will save to
      * the backwards-compatible 'Folders' section in the config file.
      */
    void setSaveBackwardsCompatible(bool save);

    /**
     * Sets up this folder's folderWatcher if possible.
     *
     * May be called several times.
     */
    void registerFolderWatcher();

signals:
    void syncStateChange();
    void syncStarted();
    void syncFinished(const SyncResult &result);
    void progressInfo(const ProgressInfo &progress);
    void newBigObjectDiscovered(const QString &, bool isFolder); // A new object bigger than the threshold was discovered
    void syncPausedChanged(Folder *, bool paused);
    void canSyncChanged();

    /**
     * Fires for each change inside this folder that wasn't caused
     * by sync activity.
     */
    void watchedFileChangedExternally(const QString &path);

public slots:

    /**
       * terminate the current sync run
       */
    void slotTerminateSync();

    // connected to the corresponding signals in the SyncEngine
    void slotAboutToRemoveAllFiles(SyncFileItem::Direction, bool *);
    void slotAboutToRestoreBackup(bool *);


    /**
      * Starts a sync operation
      *
      * If the list of changed files is known, it is passed.
      */
    void startSync(const QStringList &pathList = QStringList());

    int slotDiscardDownloadProgress();
    int downloadInfoCount();
    int slotWipeErrorBlacklist();
    int errorBlackListEntryCount();

    /**
       * Triggered by the folder watcher when a file/dir in this folder
       * changes. Needs to check whether this change should trigger a new
       * sync run to be scheduled.
       */
    void slotWatchedPathChanged(const QString &path);

private slots:
    void slotSyncStarted();
    void slotSyncFinished(bool);

    /** Adds a error message that's not tied to a specific item.
     */
    void slotSyncError(const QString &message, ErrorCategory category = ErrorCategory::Normal);

    void slotCsyncUnavailable();

    void slotTransmissionProgress(const ProgressInfo &pi);
    void slotItemCompleted(const SyncFileItemPtr &);

    void slotRunEtagJob();
    void etagRetreived(const QString &);
    void etagRetreivedFromSyncEngine(const QString &);

    void slotEmitFinishedDelayed();

    void slotNewBigObjectDiscovered(const QString &, bool isExternal, bool isFolder);

    void slotLogPropagationStart();

    /** Adds this folder to the list of scheduled folders in the
     *  FolderMan.
     */
    void slotScheduleThisFolder();

    /** Ensures that the next sync performs a full local discovery. */
    void slotNextSyncFullLocalDiscovery();

    /** Adjust sync result based on conflict data from IssuesWidget.
     *
     * This is pretty awkward, but IssuesWidget just keeps better track
     * of conflicts across partial local discovery.
     */
    void slotFolderConflicts(const QString &folder, const QStringList &conflictPaths);

    /** Warn users if they create a file or folder that is selective-sync excluded */
    void warnOnNewExcludedItem(const SyncJournalFileRecord &record, const QStringRef &path);

    /** Warn users about an unreliable folder watcher */
    void slotWatcherUnreliable(const QString &message);

private:
    bool reloadExcludes();

    void showSyncResultPopup();

    void checkLocalPath();

    void setSyncOptions();

    enum LogStatus {
        LogStatusRemove,
        LogStatusRename,
        LogStatusMove,
        LogStatusNew,
        LogStatusError,
        LogStatusConflict,
        LogStatusUpdated,
        LogStatusFileLocked
    };

    void createGuiLog(const QString &filename, LogStatus status, int count,
        const QString &renameTarget = QString());

    AccountStatePtr _accountState;
    FolderDefinition _definition;
    QString _canonicalLocalPath; // As returned with QFileInfo:canonicalFilePath.  Always ends with "/"

    SyncResult _syncResult;
    QScopedPointer<SyncEngine> _engine;
    bool _csyncUnavail;
    QPointer<RequestEtagJob> _requestEtagJob;
    QString _lastEtag;
    QElapsedTimer _timeSinceLastSyncDone;
    QElapsedTimer _timeSinceLastSyncStart;
    QElapsedTimer _timeSinceLastFullLocalDiscovery;
    std::chrono::milliseconds _lastSyncDuration;

    /// The number of syncs that failed in a row.
    /// Reset when a sync is successful.
    int _consecutiveFailingSyncs;

    /// The number of requested follow-up syncs.
    /// Reset when no follow-up is requested.
    int _consecutiveFollowUpSyncs;

    SyncJournalDb _journal;

    QScopedPointer<SyncRunFileLog> _fileLog;

    QTimer _scheduleSelfTimer;

    /**
     * When the same local path is synced to multiple accounts, only one
     * of them can be stored in the settings in a way that's compatible
     * with old clients that don't support it. This flag marks folders
     * that shall be written in a backwards-compatible way, by being set
     * on the *first* Folder instance that was configured for each local
     * path.
     */
    bool _saveBackwardsCompatible;

    /**
     * Watches this folder's local directory for changes.
     *
     * Created by registerFolderWatcher(), triggers slotWatchedPathChanged()
     */
    QScopedPointer<FolderWatcher> _folderWatcher;

    /**
     * The paths that should be checked by the next local discovery.
     *
     * Mostly a collection of files the filewatchers have reported as touched.
     * Also includes files that have had errors in the last sync run.
     */
    std::set<QByteArray> _localDiscoveryPaths;

    /**
     * The paths that the current sync run used for local discovery.
     *
     * For failing syncs, this list will be merged into _localDiscoveryPaths
     * again when the sync is done to make sure everything is retried.
     */
    std::set<QByteArray> _previousLocalDiscoveryPaths;
};
}

#endif
