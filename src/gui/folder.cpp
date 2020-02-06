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
#include "config.h"

#include "account.h"
#include "accountstate.h"
#include "folder.h"
#include "folderman.h"
#include "logger.h"
#include "configfile.h"
#include "networkjobs.h"
#include "common/syncjournalfilerecord.h"
#include "syncresult.h"
#include "clientproxy.h"
#include "syncengine.h"
#include "syncrunfilelog.h"
#include "socketapi.h"
#include "theme.h"
#include "filesystem.h"

#include "creds/abstractcredentials.h"

#include <QTimer>
#include <QUrl>
#include <QDir>
#include <QSettings>

#include <QMessageBox>
#include <QPushButton>

namespace OCC {

Q_LOGGING_CATEGORY(lcFolder, "nextcloud.gui.folder", QtInfoMsg)

Folder::Folder(const FolderDefinition &definition,
    AccountState *accountState,
    QObject *parent)
    : QObject(parent)
    , _accountState(accountState)
    , _definition(definition)
    , _csyncUnavail(false)
    , _lastSyncDuration(0)
    , _consecutiveFailingSyncs(0)
    , _consecutiveFollowUpSyncs(0)
    , _journal(_definition.absoluteJournalPath())
    , _fileLog(new SyncRunFileLog)
    , _saveBackwardsCompatible(false)
{
    _timeSinceLastSyncStart.start();
    _timeSinceLastSyncDone.start();

    SyncResult::Status status = SyncResult::NotYetStarted;
    if (definition.paused) {
        status = SyncResult::Paused;
    }
    _syncResult.setStatus(status);

    // check if the local path exists
    checkLocalPath();

    _syncResult.setFolder(_definition.alias);

    _engine.reset(new SyncEngine(_accountState->account(), path(), remotePath(), &_journal));
    // pass the setting if hidden files are to be ignored, will be read in csync_update
    _engine->setIgnoreHiddenFiles(_definition.ignoreHiddenFiles);

    ConfigFile::setupDefaultExcludeFilePaths(_engine->excludedFiles());
    if (!reloadExcludes())
        qCWarning(lcFolder, "Could not read system exclude file");

    connect(_accountState.data(), &AccountState::isConnectedChanged, this, &Folder::canSyncChanged);
    connect(_engine.data(), &SyncEngine::rootEtag, this, &Folder::etagRetreivedFromSyncEngine);

    connect(_engine.data(), &SyncEngine::started, this, &Folder::slotSyncStarted, Qt::QueuedConnection);
    connect(_engine.data(), &SyncEngine::finished, this, &Folder::slotSyncFinished, Qt::QueuedConnection);
    connect(_engine.data(), &SyncEngine::csyncUnavailable, this, &Folder::slotCsyncUnavailable, Qt::QueuedConnection);

    //direct connection so the message box is blocking the sync.
    connect(_engine.data(), &SyncEngine::aboutToRemoveAllFiles,
        this, &Folder::slotAboutToRemoveAllFiles);
    connect(_engine.data(), &SyncEngine::aboutToRestoreBackup,
        this, &Folder::slotAboutToRestoreBackup);
    connect(_engine.data(), &SyncEngine::transmissionProgress, this, &Folder::slotTransmissionProgress);
    connect(_engine.data(), &SyncEngine::itemCompleted,
        this, &Folder::slotItemCompleted);
    connect(_engine.data(), &SyncEngine::newBigObject,
        this, &Folder::slotNewBigObjectDiscovered);
    connect(_engine.data(), &SyncEngine::seenLockedFile, FolderMan::instance(), &FolderMan::slotSyncOnceFileUnlocks);
    connect(_engine.data(), &SyncEngine::aboutToPropagate,
        this, &Folder::slotLogPropagationStart);
    connect(_engine.data(), &SyncEngine::syncError, this, &Folder::slotSyncError);

    _scheduleSelfTimer.setSingleShot(true);
    _scheduleSelfTimer.setInterval(SyncEngine::minimumFileAgeForUpload);
    connect(&_scheduleSelfTimer, &QTimer::timeout,
        this, &Folder::slotScheduleThisFolder);

    connect(ProgressDispatcher::instance(), &ProgressDispatcher::folderConflicts,
        this, &Folder::slotFolderConflicts);
}

Folder::~Folder()
{
    // Reset then engine first as it will abort and try to access members of the Folder
    _engine.reset();
}

void Folder::checkLocalPath()
{
    const QFileInfo fi(_definition.localPath);
    _canonicalLocalPath = fi.canonicalFilePath();
#ifdef Q_OS_MAC
    // Workaround QTBUG-55896  (Should be fixed in Qt 5.8)
    _canonicalLocalPath = _canonicalLocalPath.normalized(QString::NormalizationForm_C);
#endif
    if (_canonicalLocalPath.isEmpty()) {
        qCWarning(lcFolder) << "Broken symlink:" << _definition.localPath;
        _canonicalLocalPath = _definition.localPath;
    } else if (!_canonicalLocalPath.endsWith('/')) {
        _canonicalLocalPath.append('/');
    }

    if (fi.isDir() && fi.isReadable()) {
        qCDebug(lcFolder) << "Checked local path ok";
    } else {
        // Check directory again
        if (!FileSystem::fileExists(_definition.localPath, fi)) {
            _syncResult.appendErrorString(tr("Local folder %1 does not exist.").arg(_definition.localPath));
            _syncResult.setStatus(SyncResult::SetupError);
        } else if (!fi.isDir()) {
            _syncResult.appendErrorString(tr("%1 should be a folder but is not.").arg(_definition.localPath));
            _syncResult.setStatus(SyncResult::SetupError);
        } else if (!fi.isReadable()) {
            _syncResult.appendErrorString(tr("%1 is not readable.").arg(_definition.localPath));
            _syncResult.setStatus(SyncResult::SetupError);
        }
    }
}

QString Folder::shortGuiRemotePathOrAppName() const
{
    if (remotePath().length() > 0 && remotePath() != QLatin1String("/")) {
        QString a = QFile(remotePath()).fileName();
        if (a.startsWith('/')) {
            a = a.remove(0, 1);
        }
        return a;
    } else {
        return Theme::instance()->appNameGUI();
    }
}

QString Folder::alias() const
{
    return _definition.alias;
}

QString Folder::path() const
{
    return _canonicalLocalPath;
}

QString Folder::shortGuiLocalPath() const
{
    QString p = _definition.localPath;
    QString home = QDir::homePath();
    if (!home.endsWith('/')) {
        home.append('/');
    }
    if (p.startsWith(home)) {
        p = p.mid(home.length());
    }
    if (p.length() > 1 && p.endsWith('/')) {
        p.chop(1);
    }
    return QDir::toNativeSeparators(p);
}


bool Folder::ignoreHiddenFiles()
{
    bool re(_definition.ignoreHiddenFiles);
    return re;
}

void Folder::setIgnoreHiddenFiles(bool ignore)
{
    _definition.ignoreHiddenFiles = ignore;
}

QString Folder::cleanPath() const
{
    QString cleanedPath = QDir::cleanPath(_canonicalLocalPath);

    if (cleanedPath.length() == 3 && cleanedPath.endsWith(":/"))
        cleanedPath.remove(2, 1);

    return cleanedPath;
}

bool Folder::isBusy() const
{
    return _engine->isSyncRunning();
}

QString Folder::remotePath() const
{
    return _definition.targetPath;
}

QUrl Folder::remoteUrl() const
{
    return Utility::concatUrlPath(_accountState->account()->davUrl(), remotePath());
}

bool Folder::syncPaused() const
{
    return _definition.paused;
}

bool Folder::canSync() const
{
    return !syncPaused() && accountState()->isConnected();
}

void Folder::setSyncPaused(bool paused)
{
    if (paused == _definition.paused) {
        return;
    }

    _definition.paused = paused;
    saveToSettings();

    if (!paused) {
        setSyncState(SyncResult::NotYetStarted);
    } else {
        setSyncState(SyncResult::Paused);
    }
    emit syncPausedChanged(this, paused);
    emit syncStateChange();
    emit canSyncChanged();
}

void Folder::setSyncState(SyncResult::Status state)
{
    _syncResult.setStatus(state);
}

SyncResult Folder::syncResult() const
{
    return _syncResult;
}

void Folder::prepareToSync()
{
    _syncResult.reset();
    _syncResult.setStatus(SyncResult::NotYetStarted);
}

void Folder::slotRunEtagJob()
{
    qCInfo(lcFolder) << "Trying to check" << remoteUrl().toString() << "for changes via ETag check. (time since last sync:" << (_timeSinceLastSyncDone.elapsed() / 1000) << "s)";

    AccountPtr account = _accountState->account();

    if (_requestEtagJob) {
        qCInfo(lcFolder) << remoteUrl().toString() << "has ETag job queued, not trying to sync";
        return;
    }

    if (!canSync()) {
        qCInfo(lcFolder) << "Not syncing.  :" << remoteUrl().toString() << _definition.paused << AccountState::stateString(_accountState->state());
        return;
    }

    // Do the ordinary etag check for the root folder and schedule a
    // sync if it's different.

    _requestEtagJob = new RequestEtagJob(account, remotePath(), this);
    _requestEtagJob->setTimeout(60 * 1000);
    // check if the etag is different when retrieved
    QObject::connect(_requestEtagJob.data(), &RequestEtagJob::etagRetreived, this, &Folder::etagRetreived);
    FolderMan::instance()->slotScheduleETagJob(alias(), _requestEtagJob);
    // The _requestEtagJob is auto deleting itself on finish. Our guard pointer _requestEtagJob will then be null.
}

void Folder::etagRetreived(const QString &etag)
{
    // re-enable sync if it was disabled because network was down
    FolderMan::instance()->setSyncEnabled(true);

    if (_lastEtag != etag) {
        qCInfo(lcFolder) << "Compare etag with previous etag: last:" << _lastEtag << ", received:" << etag << "-> CHANGED";
        _lastEtag = etag;
        slotScheduleThisFolder();
    }

    _accountState->tagLastSuccessfullETagRequest();
}

void Folder::etagRetreivedFromSyncEngine(const QString &etag)
{
    qCInfo(lcFolder) << "Root etag from during sync:" << etag;
    accountState()->tagLastSuccessfullETagRequest();
    _lastEtag = etag;
}


void Folder::showSyncResultPopup()
{
    if (_syncResult.firstItemNew()) {
        createGuiLog(_syncResult.firstItemNew()->_file, LogStatusNew, _syncResult.numNewItems());
    }
    if (_syncResult.firstItemDeleted()) {
        createGuiLog(_syncResult.firstItemDeleted()->_file, LogStatusRemove, _syncResult.numRemovedItems());
    }
    if (_syncResult.firstItemUpdated()) {
        createGuiLog(_syncResult.firstItemUpdated()->_file, LogStatusUpdated, _syncResult.numUpdatedItems());
    }

    if (_syncResult.firstItemRenamed()) {
        LogStatus status(LogStatusRename);
        // if the path changes it's rather a move
        QDir renTarget = QFileInfo(_syncResult.firstItemRenamed()->_renameTarget).dir();
        QDir renSource = QFileInfo(_syncResult.firstItemRenamed()->_file).dir();
        if (renTarget != renSource) {
            status = LogStatusMove;
        }
        createGuiLog(_syncResult.firstItemRenamed()->_originalFile, status,
            _syncResult.numRenamedItems(), _syncResult.firstItemRenamed()->_renameTarget);
    }

    if (_syncResult.firstNewConflictItem()) {
        createGuiLog(_syncResult.firstNewConflictItem()->_file, LogStatusConflict, _syncResult.numNewConflictItems());
    }
    if (int errorCount = _syncResult.numErrorItems()) {
        createGuiLog(_syncResult.firstItemError()->_file, LogStatusError, errorCount);
    }

    if (int lockedCount = _syncResult.numLockedItems()) {
        createGuiLog(_syncResult.firstItemLocked()->_file, LogStatusFileLocked, lockedCount);
    }

    qCInfo(lcFolder) << "Folder sync result: " << int(_syncResult.status());
}

void Folder::createGuiLog(const QString &filename, LogStatus status, int count,
    const QString &renameTarget)
{
    if (count > 0) {
        Logger *logger = Logger::instance();

        QString file = QDir::toNativeSeparators(filename);
        QString text;

        switch (status) {
        case LogStatusRemove:
            if (count > 1) {
                text = tr("%1 and %n other file(s) have been removed.", "", count - 1).arg(file);
            } else {
                text = tr("%1 has been removed.", "%1 names a file.").arg(file);
            }
            break;
        case LogStatusNew:
            if (count > 1) {
                text = tr("%1 and %n other file(s) have been downloaded.", "", count - 1).arg(file);
            } else {
                text = tr("%1 has been downloaded.", "%1 names a file.").arg(file);
            }
            break;
        case LogStatusUpdated:
            if (count > 1) {
                text = tr("%1 and %n other file(s) have been updated.", "", count - 1).arg(file);
            } else {
                text = tr("%1 has been updated.", "%1 names a file.").arg(file);
            }
            break;
        case LogStatusRename:
            if (count > 1) {
                text = tr("%1 has been renamed to %2 and %n other file(s) have been renamed.", "", count - 1).arg(file, renameTarget);
            } else {
                text = tr("%1 has been renamed to %2.", "%1 and %2 name files.").arg(file, renameTarget);
            }
            break;
        case LogStatusMove:
            if (count > 1) {
                text = tr("%1 has been moved to %2 and %n other file(s) have been moved.", "", count - 1).arg(file, renameTarget);
            } else {
                text = tr("%1 has been moved to %2.").arg(file, renameTarget);
            }
            break;
        case LogStatusConflict:
            if (count > 1) {
                text = tr("%1 has and %n other file(s) have sync conflicts.", "", count - 1).arg(file);
            } else {
                text = tr("%1 has a sync conflict. Please check the conflict file!").arg(file);
            }
            break;
        case LogStatusError:
            if (count > 1) {
                text = tr("%1 and %n other file(s) could not be synced due to errors. See the log for details.", "", count - 1).arg(file);
            } else {
                text = tr("%1 could not be synced due to an error. See the log for details.").arg(file);
            }
            break;
        case LogStatusFileLocked:
            if (count > 1) {
                text = tr("%1 and %n other file(s) are currently locked.", "", count -1).arg(file);
            } else {
                text = tr("%1 is currently locked.").arg(file);
            }
            break;
        }

        if (!text.isEmpty()) {
            // Ignores the settings in case of an error or conflict
            if(status == LogStatusError || status == LogStatusConflict)
                logger->postOptionalGuiLog(tr("Sync Activity"), text);
        }
    }
}

int Folder::slotDiscardDownloadProgress()
{
    // Delete from journal and from filesystem.
    QDir folderpath(_definition.localPath);
    QSet<QString> keep_nothing;
    const QVector<SyncJournalDb::DownloadInfo> deleted_infos =
        _journal.getAndDeleteStaleDownloadInfos(keep_nothing);
    foreach (const SyncJournalDb::DownloadInfo &deleted_info, deleted_infos) {
        const QString tmppath = folderpath.filePath(deleted_info._tmpfile);
        qCInfo(lcFolder) << "Deleting temporary file: " << tmppath;
        FileSystem::remove(tmppath);
    }
    return deleted_infos.size();
}

int Folder::downloadInfoCount()
{
    return _journal.downloadInfoCount();
}

int Folder::errorBlackListEntryCount()
{
    return _journal.errorBlackListEntryCount();
}

int Folder::slotWipeErrorBlacklist()
{
    return _journal.wipeErrorBlacklist();
}

void Folder::slotWatchedPathChanged(const QString &path)
{
    if (!path.startsWith(this->path())) {
        qCDebug(lcFolder) << "Changed path is not contained in folder, ignoring:" << path;
        return;
    }

    auto relativePath = path.midRef(this->path().size());

    // Add to list of locally modified paths
    //
    // We do this before checking for our own sync-related changes to make
    // extra sure to not miss relevant changes.
    auto relativePathBytes = relativePath.toUtf8();
    _localDiscoveryPaths.insert(relativePathBytes);
    qCDebug(lcFolder) << "local discovery: inserted" << relativePath << "due to file watcher";

// The folder watcher fires a lot of bogus notifications during
// a sync operation, both for actual user files and the database
// and log. Therefore we check notifications against operations
// the sync is doing to filter out our own changes.
#ifdef Q_OS_MAC
// On OSX the folder watcher does not report changes done by our
// own process. Therefore nothing needs to be done here!
#else
    // Use the path to figure out whether it was our own change
    if (_engine->wasFileTouched(path)) {
        qCDebug(lcFolder) << "Changed path was touched by SyncEngine, ignoring:" << path;
        return;
    }
#endif

    // Check that the mtime actually changed.
    SyncJournalFileRecord record;
    if (_journal.getFileRecord(relativePathBytes, &record)
        && record.isValid()
        && !FileSystem::fileChanged(path, record._fileSize, record._modtime)) {
        qCInfo(lcFolder) << "Ignoring spurious notification for file" << relativePath;
        return; // probably a spurious notification
    }

    warnOnNewExcludedItem(record, relativePath);

    emit watchedFileChangedExternally(path);

    // Also schedule this folder for a sync, but only after some delay:
    // The sync will not upload files that were changed too recently.
    scheduleThisFolderSoon();
}

void Folder::saveToSettings() const
{
    // Remove first to make sure we don't get duplicates
    removeFromSettings();

    auto settings = _accountState->settings();

    // The folder is saved to backwards-compatible "Folders"
    // section only if it has the migrate flag set (i.e. was in
    // there before) or if the folder is the only one for the
    // given target path.
    // This ensures that older clients will not read a configuration
    // where two folders for different accounts point at the same
    // local folders.
    bool oneAccountOnly = true;
    foreach (Folder *other, FolderMan::instance()->map()) {
        if (other != this && other->cleanPath() == this->cleanPath()) {
            oneAccountOnly = false;
            break;
        }
    }

    bool compatible = _saveBackwardsCompatible || oneAccountOnly;

    if (compatible) {
        settings->beginGroup(QLatin1String("Folders"));
    } else {
        settings->beginGroup(QLatin1String("Multifolders"));
    }
    FolderDefinition::save(*settings, _definition);

    settings->sync();
    qCInfo(lcFolder) << "Saved folder" << _definition.alias << "to settings, status" << settings->status();
}

void Folder::removeFromSettings() const
{
    auto settings = _accountState->settings();
    settings->beginGroup(QLatin1String("Folders"));
    settings->remove(FolderMan::escapeAlias(_definition.alias));
    settings->endGroup();
    settings->beginGroup(QLatin1String("Multifolders"));
    settings->remove(FolderMan::escapeAlias(_definition.alias));
}

bool Folder::isFileExcludedAbsolute(const QString &fullPath) const
{
    return _engine->excludedFiles().isExcluded(fullPath, path(), _definition.ignoreHiddenFiles);
}

bool Folder::isFileExcludedRelative(const QString &relativePath) const
{
    return _engine->excludedFiles().isExcluded(path() + relativePath, path(), _definition.ignoreHiddenFiles);
}

void Folder::slotTerminateSync()
{
    qCInfo(lcFolder) << "folder " << alias() << " Terminating!";

    if (_engine->isSyncRunning()) {
        _engine->abort();

        setSyncState(SyncResult::SyncAbortRequested);
    }
}

// This removes the csync File database
// This is needed to provide a clean startup again in case another
// local folder is synced to the same ownCloud.
void Folder::wipe()
{
    QString stateDbFile = _engine->journal()->databaseFilePath();

    // Delete files that have been partially downloaded.
    slotDiscardDownloadProgress();

    //Unregister the socket API so it does not keep the ._sync_journal file open
    FolderMan::instance()->socketApi()->slotUnregisterPath(alias());
    _journal.close(); // close the sync journal

    QFile file(stateDbFile);
    if (file.exists()) {
        if (!file.remove()) {
            qCWarning(lcFolder) << "Failed to remove existing csync StateDB " << stateDbFile;
        } else {
            qCInfo(lcFolder) << "wipe: Removed csync StateDB " << stateDbFile;
        }
    } else {
        qCWarning(lcFolder) << "statedb is empty, can not remove.";
    }

    // Also remove other db related files
    QFile::remove(stateDbFile + ".ctmp");
    QFile::remove(stateDbFile + "-shm");
    QFile::remove(stateDbFile + "-wal");
    QFile::remove(stateDbFile + "-journal");

    if (canSync())
        FolderMan::instance()->socketApi()->slotRegisterPath(alias());
}

bool Folder::reloadExcludes()
{
    return _engine->excludedFiles().reloadExcludeFiles();
}

void Folder::startSync(const QStringList &pathList)
{
    Q_UNUSED(pathList)

    if (isBusy()) {
        qCCritical(lcFolder) << "ERROR csync is still running and new sync requested.";
        return;
    }
    _csyncUnavail = false;

    _timeSinceLastSyncStart.start();
    _syncResult.setStatus(SyncResult::SyncPrepare);
    emit syncStateChange();

    qCInfo(lcFolder) << "*** Start syncing " << remoteUrl().toString() << " - client version"
                     << qPrintable(Theme::instance()->version());

    _fileLog->start(path());

    if (!reloadExcludes()) {
        slotSyncError(tr("Could not read system exclude file"));
        QMetaObject::invokeMethod(this, "slotSyncFinished", Qt::QueuedConnection, Q_ARG(bool, false));
        return;
    }

    setDirtyNetworkLimits();
    setSyncOptions();

    static std::chrono::milliseconds fullLocalDiscoveryInterval = []() {
        auto interval = ConfigFile().fullLocalDiscoveryInterval();
        QByteArray env = qgetenv("OWNCLOUD_FULL_LOCAL_DISCOVERY_INTERVAL");
        if (!env.isEmpty()) {
            interval = std::chrono::milliseconds(env.toLongLong());
        }
        return interval;
    }();
    bool hasDoneFullLocalDiscovery = _timeSinceLastFullLocalDiscovery.isValid();
    bool periodicFullLocalDiscoveryNow =
        fullLocalDiscoveryInterval.count() >= 0 // negative means we don't require periodic full runs
        && _timeSinceLastFullLocalDiscovery.hasExpired(fullLocalDiscoveryInterval.count());
    if (_folderWatcher && _folderWatcher->isReliable()
        && hasDoneFullLocalDiscovery
        && !periodicFullLocalDiscoveryNow) {
        qCInfo(lcFolder) << "Allowing local discovery to read from the database";
        _engine->setLocalDiscoveryOptions(LocalDiscoveryStyle::DatabaseAndFilesystem, _localDiscoveryPaths);

        if (lcFolder().isDebugEnabled()) {
            QByteArrayList paths;
            for (auto &path : _localDiscoveryPaths)
                paths.append(path);
            qCDebug(lcFolder) << "local discovery paths: " << paths;
        }

        _previousLocalDiscoveryPaths = std::move(_localDiscoveryPaths);
    } else {
        qCInfo(lcFolder) << "Forbidding local discovery to read from the database";
        _engine->setLocalDiscoveryOptions(LocalDiscoveryStyle::FilesystemOnly);
        _previousLocalDiscoveryPaths.clear();
    }
    _localDiscoveryPaths.clear();

    _engine->setIgnoreHiddenFiles(_definition.ignoreHiddenFiles);

    QMetaObject::invokeMethod(_engine.data(), "startSync", Qt::QueuedConnection);

    emit syncStarted();
}

void Folder::setSyncOptions()
{
    SyncOptions opt;
    ConfigFile cfgFile;

    auto newSizeLimit = cfgFile.newBigFolderSizeLimit();
    opt._newBigFolderSizeLimit = newSizeLimit.first ? newSizeLimit.second * 1000LL * 1000LL : -1; // convert from MB to B
    opt._confirmExternalStorage = cfgFile.confirmExternalStorage();
    opt._moveFilesToTrash = cfgFile.moveToTrash();

    QByteArray chunkSizeEnv = qgetenv("OWNCLOUD_CHUNK_SIZE");
    if (!chunkSizeEnv.isEmpty()) {
        opt._initialChunkSize = chunkSizeEnv.toUInt();
    } else {
        opt._initialChunkSize = cfgFile.chunkSize();
    }
    QByteArray minChunkSizeEnv = qgetenv("OWNCLOUD_MIN_CHUNK_SIZE");
    if (!minChunkSizeEnv.isEmpty()) {
        opt._minChunkSize = minChunkSizeEnv.toUInt();
    } else {
        opt._minChunkSize = cfgFile.minChunkSize();
    }
    QByteArray maxChunkSizeEnv = qgetenv("OWNCLOUD_MAX_CHUNK_SIZE");
    if (!maxChunkSizeEnv.isEmpty()) {
        opt._maxChunkSize = maxChunkSizeEnv.toUInt();
    } else {
        opt._maxChunkSize = cfgFile.maxChunkSize();
    }

    // Previously min/max chunk size values didn't exist, so users might
    // have setups where the chunk size exceeds the new min/max default
    // values. To cope with this, adjust min/max to always include the
    // initial chunk size value.
    opt._minChunkSize = qMin(opt._minChunkSize, opt._initialChunkSize);
    opt._maxChunkSize = qMax(opt._maxChunkSize, opt._initialChunkSize);

    QByteArray targetChunkUploadDurationEnv = qgetenv("OWNCLOUD_TARGET_CHUNK_UPLOAD_DURATION");
    if (!targetChunkUploadDurationEnv.isEmpty()) {
        opt._targetChunkUploadDuration = std::chrono::milliseconds(targetChunkUploadDurationEnv.toUInt());
    } else {
        opt._targetChunkUploadDuration = cfgFile.targetChunkUploadDuration();
    }

    _engine->setSyncOptions(opt);
}

void Folder::setDirtyNetworkLimits()
{
    ConfigFile cfg;
    int downloadLimit = -75; // 75%
    int useDownLimit = cfg.useDownloadLimit();
    if (useDownLimit >= 1) {
        downloadLimit = cfg.downloadLimit() * 1000;
    } else if (useDownLimit == 0) {
        downloadLimit = 0;
    }

    int uploadLimit = -75; // 75%
    int useUpLimit = cfg.useUploadLimit();
    if (useUpLimit >= 1) {
        uploadLimit = cfg.uploadLimit() * 1000;
    } else if (useUpLimit == 0) {
        uploadLimit = 0;
    }

    _engine->setNetworkLimits(uploadLimit, downloadLimit);
}

void Folder::slotSyncError(const QString &message, ErrorCategory category)
{
    _syncResult.appendErrorString(message);
    emit ProgressDispatcher::instance()->syncError(alias(), message, category);
}

void Folder::slotSyncStarted()
{
    qCInfo(lcFolder) << "#### Propagation start ####################################################";
    _syncResult.setStatus(SyncResult::SyncRunning);
    emit syncStateChange();
}

void Folder::slotCsyncUnavailable()
{
    _csyncUnavail = true;
}

void Folder::slotSyncFinished(bool success)
{
    qCInfo(lcFolder) << "Client version" << qPrintable(Theme::instance()->version())
                     << " Qt" << qVersion()
                     << " SSL " << QSslSocket::sslLibraryVersionString().toUtf8().data()
        ;

    bool syncError = !_syncResult.errorStrings().isEmpty();
    if (syncError) {
        qCWarning(lcFolder) << "SyncEngine finished with ERROR";
    } else {
        qCInfo(lcFolder) << "SyncEngine finished without problem.";
    }
    _fileLog->finish();
    showSyncResultPopup();

    auto anotherSyncNeeded = _engine->isAnotherSyncNeeded();

    if (syncError) {
        _syncResult.setStatus(SyncResult::Error);
    } else if (_csyncUnavail) {
        _syncResult.setStatus(SyncResult::Error);
        qCWarning(lcFolder) << "csync not available.";
    } else if (_syncResult.foundFilesNotSynced()) {
        _syncResult.setStatus(SyncResult::Problem);
    } else if (_definition.paused) {
        // Maybe the sync was terminated because the user paused the folder
        _syncResult.setStatus(SyncResult::Paused);
    } else {
        _syncResult.setStatus(SyncResult::Success);
    }

    // Count the number of syncs that have failed in a row.
    if (_syncResult.status() == SyncResult::Success
        || _syncResult.status() == SyncResult::Problem) {
        _consecutiveFailingSyncs = 0;
    } else {
        _consecutiveFailingSyncs++;
        qCInfo(lcFolder) << "the last" << _consecutiveFailingSyncs << "syncs failed";
    }

    if (_syncResult.status() == SyncResult::Success && success) {
        // Clear the white list as all the folders that should be on that list are sync-ed
        journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncWhiteList, QStringList());
    }

    // bug: This function uses many different criteria for "sync was successful" - investigate!
    if ((_syncResult.status() == SyncResult::Success
            || _syncResult.status() == SyncResult::Problem)
        && success) {
        if (_engine->lastLocalDiscoveryStyle() == LocalDiscoveryStyle::FilesystemOnly) {
            _timeSinceLastFullLocalDiscovery.start();
        }
        qCDebug(lcFolder) << "Sync success, forgetting last sync's local discovery path list";
    } else {
        // On overall-failure we can't forget about last sync's local discovery
        // paths yet, reuse them for the next sync again.
        // C++17: Could use std::set::merge().
        _localDiscoveryPaths.insert(
            _previousLocalDiscoveryPaths.begin(), _previousLocalDiscoveryPaths.end());
        qCDebug(lcFolder) << "Sync failed, keeping last sync's local discovery path list";
    }
    _previousLocalDiscoveryPaths.clear();

    emit syncStateChange();

    // The syncFinished result that is to be triggered here makes the folderman
    // clear the current running sync folder marker.
    // Lets wait a bit to do that because, as long as this marker is not cleared,
    // file system change notifications are ignored for that folder. And it takes
    // some time under certain conditions to make the file system notifications
    // all come in.
    QTimer::singleShot(200, this, &Folder::slotEmitFinishedDelayed);

    _lastSyncDuration = std::chrono::milliseconds(_timeSinceLastSyncStart.elapsed());
    _timeSinceLastSyncDone.start();

    // Increment the follow-up sync counter if necessary.
    if (anotherSyncNeeded == ImmediateFollowUp) {
        _consecutiveFollowUpSyncs++;
        qCInfo(lcFolder) << "another sync was requested by the finished sync, this has"
                         << "happened" << _consecutiveFollowUpSyncs << "times";
    } else {
        _consecutiveFollowUpSyncs = 0;
    }

    // Maybe force a follow-up sync to take place, but only a couple of times.
    if (anotherSyncNeeded == ImmediateFollowUp && _consecutiveFollowUpSyncs <= 3) {
        // Sometimes another sync is requested because a local file is still
        // changing, so wait at least a small amount of time before syncing
        // the folder again.
        scheduleThisFolderSoon();
    }
}

void Folder::slotEmitFinishedDelayed()
{
    emit syncFinished(_syncResult);

    // Immediately check the etag again if there was some sync activity.
    if ((_syncResult.status() == SyncResult::Success
            || _syncResult.status() == SyncResult::Problem)
        && (_syncResult.firstItemDeleted()
               || _syncResult.firstItemNew()
               || _syncResult.firstItemRenamed()
               || _syncResult.firstItemUpdated()
               || _syncResult.firstNewConflictItem())) {
        slotRunEtagJob();
    }
}

// the progress comes without a folder and the valid path set. Add that here
// and hand the result over to the progress dispatcher.
void Folder::slotTransmissionProgress(const ProgressInfo &pi)
{
    emit progressInfo(pi);
    ProgressDispatcher::instance()->setProgressInfo(alias(), pi);
}

// a item is completed: count the errors and forward to the ProgressDispatcher
void Folder::slotItemCompleted(const SyncFileItemPtr &item)
{
    if (item->_instruction == CSYNC_INSTRUCTION_NONE || item->_instruction == CSYNC_INSTRUCTION_UPDATE_METADATA) {
        // We only care about the updates that deserve to be shown in the UI
        return;
    }

    // add new directories or remove gone away dirs to the watcher
    if (_folderWatcher && item->isDirectory()) {
	switch (item->_instruction) {
	    case CSYNC_INSTRUCTION_NEW:
                _folderWatcher->addPath(path() + item->_file);
	    break;
	    case CSYNC_INSTRUCTION_REMOVE:
                _folderWatcher->removePath(path() + item->_file);
	    break;
	    case CSYNC_INSTRUCTION_RENAME:
                _folderWatcher->removePath(path() + item->_file);
                _folderWatcher->addPath(path() + item->destination());
	    break;
        default:
        break;
	}
    }

    // Success and failure of sync items adjust what the next sync is
    // supposed to do.
    //
    // For successes, we want to wipe the file from the list to ensure we don't
    // rediscover it even if this overall sync fails.
    //
    // For failures, we want to add the file to the list so the next sync
    // will be able to retry it.
    if (item->_status == SyncFileItem::Success
        || item->_status == SyncFileItem::FileIgnored
        || item->_status == SyncFileItem::Restoration
        || item->_status == SyncFileItem::Conflict) {
        if (_previousLocalDiscoveryPaths.erase(item->_file.toUtf8()))
            qCDebug(lcFolder) << "local discovery: wiped" << item->_file;
    } else {
        _localDiscoveryPaths.insert(item->_file.toUtf8());
        qCDebug(lcFolder) << "local discovery: inserted" << item->_file << "due to sync failure";
    }

    _syncResult.processCompletedItem(item);

    _fileLog->logItem(*item);
    emit ProgressDispatcher::instance()->itemCompleted(alias(), item);
}

void Folder::slotNewBigObjectDiscovered(const QString &newO, bool isExternal, bool isFolder)
{
    auto newObject = newO;

    if (isFolder && !newObject.endsWith(QLatin1Char('/'))) {
        newObject += QLatin1Char('/');
    }
    auto journal = journalDb();

    // Add the entry to the blacklist if it is neither in the blacklist or whitelist already
    bool ok1, ok2;
    auto blacklist = journal->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, &ok1);
    auto whitelist = journal->getSelectiveSyncList(SyncJournalDb::SelectiveSyncWhiteList, &ok2);
    if (ok1 && ok2 && !blacklist.contains(newObject) && !whitelist.contains(newObject)) {
        blacklist.append(newObject);
        journal->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, blacklist);
    }

    // And add the entry to the undecided list and signal the UI
    auto undecidedList = journal->getSelectiveSyncList(SyncJournalDb::SelectiveSyncUndecidedList, &ok1);
    if (ok1) {
        if (!undecidedList.contains(newObject)) {
            undecidedList.append(newObject);
            journal->setSelectiveSyncList(SyncJournalDb::SelectiveSyncUndecidedList, undecidedList);
            emit newBigObjectDiscovered(newObject, isFolder);
        }
		
		QString message = "";
		if (isFolder) {
            message = !isExternal ? (tr("A new folder larger than %1 MB has been added: %2.\n")
                                                 .arg(ConfigFile().newBigFolderSizeLimit().second)
                                                 .arg(newO))
                                          : (tr("A folder from an external storage has been added.\n"));
            message += tr("Please go in the settings to select it if you wish to download it.");
        } else {
            message = !isExternal ? (tr("A new file larger than %1 MB has been added: %2.\n")
                                                    .arg(ConfigFile().newBigFolderSizeLimit().second)
                                                    .arg(newO))
                                            : (tr("A file from an external storage has been added.\n"));
            message += tr("Please go in the settings to select it if you wish to download it.");
		}

        auto logger = Logger::instance();
        logger->postOptionalGuiLog(Theme::instance()->appNameGUI(), message);
    }
}

void Folder::slotLogPropagationStart()
{
    _fileLog->logLap("Propagation starts");
}

void Folder::slotScheduleThisFolder()
{
    FolderMan::instance()->scheduleFolder(this);
}

void Folder::slotNextSyncFullLocalDiscovery()
{
    _timeSinceLastFullLocalDiscovery.invalidate();
}

void Folder::slotFolderConflicts(const QString &folder, const QStringList &conflictPaths)
{
    if (folder != _definition.alias)
        return;
    auto &r = _syncResult;

    // If the number of conflicts is too low, adjust it upwards
    if (conflictPaths.size() > r.numNewConflictItems() + r.numOldConflictItems())
        r.setNumOldConflictItems(conflictPaths.size() - r.numNewConflictItems());
}

void Folder::warnOnNewExcludedItem(const SyncJournalFileRecord &record, const QStringRef &path)
{
    // Never warn for items in the database
    if (record.isValid())
        return;

    // Don't warn for items that no longer exist.
    // Note: This assumes we're getting file watcher notifications
    // for folders only on creation and deletion - if we got a notification
    // on content change that would create spurious warnings.
    QFileInfo fi(_canonicalLocalPath + path);
    if (!fi.exists())
        return;

    bool ok = false;
    auto blacklist = _journal.getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, &ok);
    if (!ok)
        return;
    if (!blacklist.contains(path + "/"))
        return;

    const auto message = fi.isDir()
        ? tr("The folder %1 was created but was excluded from synchronization previously. "
             "Data inside it will not be synchronized.")
              .arg(fi.filePath())
        : tr("The file %1 was created but was excluded from synchronization previously. "
             "It will not be synchronized.")
              .arg(fi.filePath());

    Logger::instance()->postOptionalGuiLog(Theme::instance()->appNameGUI(), message);
}

void Folder::slotWatcherUnreliable(const QString &message)
{
    qCWarning(lcFolder) << "Folder watcher for" << path() << "became unreliable:" << message;
    auto fullMessage =
        tr("Changes in synchronized folders could not be tracked reliably.\n"
           "\n"
           "This means that the synchronization client might not upload local changes "
           "immediately and will instead only scan for local changes and upload them "
           "occasionally (every two hours by default).\n"
           "\n"
           "%1").arg(message);
    Logger::instance()->postGuiLog(Theme::instance()->appNameGUI(), fullMessage);
}

void Folder::scheduleThisFolderSoon()
{
    if (!_scheduleSelfTimer.isActive()) {
        _scheduleSelfTimer.start();
    }
}

void Folder::setSaveBackwardsCompatible(bool save)
{
    _saveBackwardsCompatible = save;
}

void Folder::registerFolderWatcher()
{
    if (_folderWatcher)
        return;
    if (!QDir(path()).exists())
        return;

    _folderWatcher.reset(new FolderWatcher(this));
    connect(_folderWatcher.data(), &FolderWatcher::pathChanged,
        this, &Folder::slotWatchedPathChanged);
    connect(_folderWatcher.data(), &FolderWatcher::lostChanges,
        this, &Folder::slotNextSyncFullLocalDiscovery);
    connect(_folderWatcher.data(), &FolderWatcher::becameUnreliable,
        this, &Folder::slotWatcherUnreliable);
    _folderWatcher->init(path());
}

void Folder::slotAboutToRemoveAllFiles(SyncFileItem::Direction dir, bool *cancel)
{
    ConfigFile cfgFile;
    if (!cfgFile.promptDeleteFiles())
        return;

    QString msg = dir == SyncFileItem::Down ? tr("All files in the sync folder '%1' were deleted on the server.\n"
                                                 "These deletes will be synchronized to your local sync folder, making such files "
                                                 "unavailable unless you have a right to restore. \n"
                                                 "If you decide to restore the files, they will be re-synced with the server if you have rights to do so.\n"
                                                 "If you decide to delete the files, they will be unavailable to you, unless you are the owner.")
                                            : tr("All files got deleted from your local sync folder '%1'.\n"
                                                 "These files will be deleted from the server and will not be available on your other devices if they "
                                                 "will not be restored.\n"
                                                 "If this action was unintended you can restore the lost data now.");
    QMessageBox msgBox(QMessageBox::Warning, tr("Delete all files?"),
        msg.arg(shortGuiLocalPath()));
    msgBox.setWindowFlags(msgBox.windowFlags() | Qt::WindowStaysOnTopHint);
    msgBox.addButton(tr("Delete all files"), QMessageBox::DestructiveRole);
    QPushButton *keepBtn = msgBox.addButton(tr("Restore deleted files"), QMessageBox::AcceptRole);
    if (msgBox.exec() == -1) {
        *cancel = true;
        return;
    }
    *cancel = msgBox.clickedButton() == keepBtn;
    if (*cancel) {
        FileSystem::setFolderMinimumPermissions(path());
        journalDb()->clearFileTable();
        _lastEtag.clear();
        slotScheduleThisFolder();
    }
}

void Folder::slotAboutToRestoreBackup(bool *restore)
{
    QString msg =
        tr("This sync would reset the files to an earlier time in the sync folder '%1'.\n"
           "This might be because a backup was restored on the server.\n"
           "Continuing the sync as normal will cause all your files to be overwritten by an older "
           "file in an earlier state. "
           "Do you want to keep your local most recent files as conflict files?");
    QMessageBox msgBox(QMessageBox::Warning, tr("Backup detected"),
        msg.arg(shortGuiLocalPath()));
    msgBox.setWindowFlags(msgBox.windowFlags() | Qt::WindowStaysOnTopHint);
    msgBox.addButton(tr("Normal Synchronisation"), QMessageBox::DestructiveRole);
    QPushButton *keepBtn = msgBox.addButton(tr("Keep Local Files as Conflict"), QMessageBox::AcceptRole);

    if (msgBox.exec() == -1) {
        *restore = true;
        return;
    }
    *restore = msgBox.clickedButton() == keepBtn;
}


void FolderDefinition::save(QSettings &settings, const FolderDefinition &folder)
{
    settings.beginGroup(FolderMan::escapeAlias(folder.alias));
    settings.setValue(QLatin1String("localPath"), folder.localPath);
    settings.setValue(QLatin1String("journalPath"), folder.journalPath);
    settings.setValue(QLatin1String("targetPath"), folder.targetPath);
    settings.setValue(QLatin1String("paused"), folder.paused);
    settings.setValue(QLatin1String("ignoreHiddenFiles"), folder.ignoreHiddenFiles);

    // Happens only on Windows when the explorer integration is enabled.
    if (!folder.navigationPaneClsid.isNull())
        settings.setValue(QLatin1String("navigationPaneClsid"), folder.navigationPaneClsid);
    else
        settings.remove(QLatin1String("navigationPaneClsid"));
    settings.endGroup();
}

bool FolderDefinition::load(QSettings &settings, const QString &alias,
    FolderDefinition *folder)
{
    settings.beginGroup(alias);
    folder->alias = FolderMan::unescapeAlias(alias);
    folder->localPath = settings.value(QLatin1String("localPath")).toString();
    folder->journalPath = settings.value(QLatin1String("journalPath")).toString();
    folder->targetPath = settings.value(QLatin1String("targetPath")).toString();
    folder->paused = settings.value(QLatin1String("paused")).toBool();
    folder->ignoreHiddenFiles = settings.value(QLatin1String("ignoreHiddenFiles"), QVariant(true)).toBool();
    folder->navigationPaneClsid = settings.value(QLatin1String("navigationPaneClsid")).toUuid();
    settings.endGroup();

    // Old settings can contain paths with native separators. In the rest of the
    // code we assum /, so clean it up now.
    folder->localPath = prepareLocalPath(folder->localPath);

    // Target paths also have a convention
    folder->targetPath = prepareTargetPath(folder->targetPath);

    return true;
}

QString FolderDefinition::prepareLocalPath(const QString &path)
{
    QString p = QDir::fromNativeSeparators(path);
    if (!p.endsWith(QLatin1Char('/'))) {
        p.append(QLatin1Char('/'));
    }
    return p;
}

QString FolderDefinition::prepareTargetPath(const QString &path)
{
    QString p = path;
    if (p.endsWith(QLatin1Char('/'))) {
        p.chop(1);
    }
    // Doing this second ensures the empty string or "/" come
    // out as "/".
    if (!p.startsWith(QLatin1Char('/'))) {
        p.prepend(QLatin1Char('/'));
    }
    return p;
}

QString FolderDefinition::absoluteJournalPath() const
{
    return QDir(localPath).filePath(journalPath);
}

QString FolderDefinition::defaultJournalPath(AccountPtr account)
{
    return SyncJournalDb::makeDbName(account->url(), targetPath, account->credentials()->user());
}

} // namespace OCC
