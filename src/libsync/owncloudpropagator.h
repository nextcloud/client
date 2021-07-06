/*
 * Copyright (C) by Olivier Goffart <ogoffart@owncloud.com>
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

#ifndef OWNCLOUDPROPAGATOR_H
#define OWNCLOUDPROPAGATOR_H

#include <QHash>
#include <QObject>
#include <QMap>
#include <QElapsedTimer>
#include <QTimer>
#include <QPointer>
#include <QIODevice>
#include <QMutex>

#include "csync.h"
#include "syncfileitem.h"
#include "common/syncjournaldb.h"
#include "bandwidthmanager.h"
#include "accountfwd.h"
#include "syncoptions.h"

namespace OCC {

Q_DECLARE_LOGGING_CATEGORY(lcPropagator)

/** Free disk space threshold below which syncs will abort and not even start.
 */
qint64 criticalFreeSpaceLimit();

/** The client will not intentionally reduce the available free disk space below
 *  this limit.
 *
 * Uploads will still run and downloads that are small enough will continue too.
 */
qint64 freeSpaceLimit();

class SyncJournalDb;
class OwncloudPropagator;
class PropagatorCompositeJob;

/**
 * @brief the base class of propagator jobs
 *
 * This can either be a job, or a container for jobs.
 * If it is a composite job, it then inherits from PropagateDirectory
 *
 * @ingroup libsync
 */
class PropagatorJob : public QObject
{
    Q_OBJECT

public:
    explicit PropagatorJob(OwncloudPropagator *propagator);

    enum AbortType {
        Synchronous,
        Asynchronous
    };

    enum JobState {
        NotYetStarted,
        Running,
        Finished
    };
    JobState _state;

    enum JobParallelism {

        /** Jobs can be run in parallel to this job */
        FullParallelism,

        /** No other job shall be started until this one has finished.
            So this job is guaranteed to finish before any jobs below it
            are executed. */
        WaitForFinished,
    };

    virtual JobParallelism parallelism() { return FullParallelism; }

    /**
     * For "small" jobs
     */
    virtual bool isLikelyFinishedQuickly() { return false; }

    /** The space that the running jobs need to complete but don't actually use yet.
     *
     * Note that this does *not* include the disk space that's already
     * in use by running jobs for things like a download-in-progress.
     */
    virtual qint64 committedDiskSpace() const { return 0; }

    /** Set the associated composite job
     *
     * Used only from PropagatorCompositeJob itself, when a job is added
     * and from PropagateDirectory to associate the subJobs with the first
     * job.
     */
    void setAssociatedComposite(PropagatorCompositeJob *job) { _associatedComposite = job; }

public slots:
    /*
     * Asynchronous abort requires emit of abortFinished() signal,
     * while synchronous is expected to abort immedietaly.
    */
    virtual void abort(PropagatorJob::AbortType abortType) {
        if (abortType == AbortType::Asynchronous)
            emit abortFinished();
    }

    /** Starts this job, or a new subjob
     * returns true if a job was started.
     */
    virtual bool scheduleSelfOrChild() = 0;
signals:
    /**
     * Emitted when the job is fully finished
     */
    void finished(SyncFileItem::Status);

    /**
     * Emitted when the abort is fully finished
     */
    void abortFinished(SyncFileItem::Status status = SyncFileItem::NormalError);
protected:
    OwncloudPropagator *propagator() const;

    /** If this job gets added to a composite job, this will point to the parent.
     *
     * For the PropagateDirectory::_firstJob it will point to
     * PropagateDirectory::_subJobs.
     *
     * That can be useful for jobs that want to spawn follow-up jobs without
     * becoming composite jobs themselves.
     */
    PropagatorCompositeJob *_associatedComposite = nullptr;
};

/*
 * Abstract class to propagate a single item
 */
class PropagateItemJob : public PropagatorJob
{
    Q_OBJECT
protected:
    virtual void done(SyncFileItem::Status status, const QString &errorString = QString());

    /*
     * set a custom restore job message that is used if the restore job succeeded.
     * It is displayed in the activity view.
     */
    QString restoreJobMsg() const
    {
        return _item->_isRestoration ? _item->_errorString : QString();
    }
    void setRestoreJobMsg(const QString &msg = QString())
    {
        _item->_isRestoration = true;
        _item->_errorString = msg;
    }

    bool hasEncryptedAncestor() const;

protected slots:
    void slotRestoreJobFinished(SyncFileItem::Status status);

private:
    QScopedPointer<PropagateItemJob> _restoreJob;
    JobParallelism _parallelism;

public:
    PropagateItemJob(OwncloudPropagator *propagator, const SyncFileItemPtr &item)
        : PropagatorJob(propagator)
        , _item(item)
        , _parallelism(FullParallelism)
    {
        // we should always execute jobs that process the E2EE API calls as sequential jobs
        // TODO: In fact, we must make sure Lock/Unlock are not colliding and always wait for each other to complete. So, we could refactor this "_parallelism" later
        // so every "PropagateItemJob" that will potentially execute Lock job on E2EE folder will get executed sequentially.
        // As an alternative, we could optimize Lock/Unlock calls, so we do a batch-write on one folder and only lock and unlock a folder once per batch.
        _parallelism = (_item->_isEncrypted || hasEncryptedAncestor()) ? WaitForFinished : FullParallelism;
    }
    ~PropagateItemJob();

    bool scheduleSelfOrChild() override
    {
        if (_state != NotYetStarted) {
            return false;
        }
        qCInfo(lcPropagator) << "Starting" << _item->_instruction << "propagation of" << _item->destination() << "by" << this;

        _state = Running;
        QMetaObject::invokeMethod(this, "start"); // We could be in a different thread (neon jobs)
        return true;
    }

    virtual JobParallelism parallelism() override { return _parallelism; }

    SyncFileItemPtr _item;

public slots:
    virtual void start() = 0;
};

/**
 * @brief Job that runs subjobs. It becomes finished only when all subjobs are finished.
 * @ingroup libsync
 */
class PropagatorCompositeJob : public PropagatorJob
{
    Q_OBJECT
public:
    QVector<PropagatorJob *> _jobsToDo;
    SyncFileItemVector _tasksToDo;
    QVector<PropagatorJob *> _runningJobs;
    SyncFileItem::Status _hasError; // NoStatus,  or NormalError / SoftError if there was an error
    quint64 _abortsCount;

    explicit PropagatorCompositeJob(OwncloudPropagator *propagator)
        : PropagatorJob(propagator)
        , _hasError(SyncFileItem::NoStatus), _abortsCount(0)
    {
    }

    // Don't delete jobs in _jobsToDo and _runningJobs: they have parents
    // that will be responsible for cleanup. Deleting them here would risk
    // deleting something that has already been deleted by a shared parent.
    virtual ~PropagatorCompositeJob() = default;

    void appendJob(PropagatorJob *job);
    void appendTask(const SyncFileItemPtr &item)
    {
        _tasksToDo.append(item);
    }

    bool scheduleSelfOrChild() override;
    JobParallelism parallelism() override;

    /*
     * Abort synchronously or asynchronously - some jobs
     * require to be finished without immediete abort (abort on job might
     * cause conflicts/duplicated files - owncloud/client/issues/5949)
     */
    void abort(PropagatorJob::AbortType abortType) override
    {
        if (!_runningJobs.empty()) {
            _abortsCount = _runningJobs.size();
            foreach (PropagatorJob *j, _runningJobs) {
                if (abortType == AbortType::Asynchronous) {
                    connect(j, &PropagatorJob::abortFinished,
                            this, &PropagatorCompositeJob::slotSubJobAbortFinished);
                }
                j->abort(abortType);
            }
        } else if (abortType == AbortType::Asynchronous){
            emit abortFinished();
        }
    }

    qint64 committedDiskSpace() const override;

private slots:
    void slotSubJobAbortFinished();
    bool possiblyRunNextJob(PropagatorJob *next)
    {
        if (next->_state == NotYetStarted) {
            connect(next, &PropagatorJob::finished, this, &PropagatorCompositeJob::slotSubJobFinished);
        }
        return next->scheduleSelfOrChild();
    }

    void slotSubJobFinished(SyncFileItem::Status status);
    void finalize();
};

/**
 * @brief Propagate a directory, and all its sub entries.
 * @ingroup libsync
 */
class OWNCLOUDSYNC_EXPORT PropagateDirectory : public PropagatorJob
{
    Q_OBJECT
public:
    SyncFileItemPtr _item;
    // e.g: create the directory
    QScopedPointer<PropagateItemJob> _firstJob;

    PropagatorCompositeJob _subJobs;

    explicit PropagateDirectory(OwncloudPropagator *propagator, const SyncFileItemPtr &item);

    void appendJob(PropagatorJob *job)
    {
        _subJobs.appendJob(job);
    }

    void appendTask(const SyncFileItemPtr &item)
    {
        _subJobs.appendTask(item);
    }

    bool scheduleSelfOrChild() override;
    JobParallelism parallelism() override;
    void abort(PropagatorJob::AbortType abortType) override
    {
        if (_firstJob)
            // Force first job to abort synchronously
            // even if caller allows async abort (asyncAbort)
            _firstJob->abort(AbortType::Synchronous);

        if (abortType == AbortType::Asynchronous){
            connect(&_subJobs, &PropagatorCompositeJob::abortFinished, this, &PropagateDirectory::abortFinished);
        }
        _subJobs.abort(abortType);
    }

    void increaseAffectedCount()
    {
        _firstJob->_item->_affectedItems++;
    }


    qint64 committedDiskSpace() const override
    {
        return _subJobs.committedDiskSpace();
    }

private slots:

    void slotFirstJobFinished(SyncFileItem::Status status);
    virtual void slotSubJobsFinished(SyncFileItem::Status status);

};

/**
 * @brief Propagate the root directory, and all its sub entries.
 * @ingroup libsync
 *
 * Primary difference to PropagateDirectory is that it keeps track of directory
 * deletions that must happen at the very end.
 */
class OWNCLOUDSYNC_EXPORT PropagateRootDirectory : public PropagateDirectory
{
    Q_OBJECT
public:
    PropagatorCompositeJob _dirDeletionJobs;

    explicit PropagateRootDirectory(OwncloudPropagator *propagator);

    bool scheduleSelfOrChild() override;
    JobParallelism parallelism() override;
    void abort(PropagatorJob::AbortType abortType) override;

    qint64 committedDiskSpace() const override;

private slots:
    void slotSubJobsFinished(SyncFileItem::Status status) override;
    void slotDirDeletionJobsFinished(SyncFileItem::Status status);
};

/**
 * @brief Dummy job that just mark it as completed and ignored
 * @ingroup libsync
 */
class PropagateIgnoreJob : public PropagateItemJob
{
    Q_OBJECT
public:
    PropagateIgnoreJob(OwncloudPropagator *propagator, const SyncFileItemPtr &item)
        : PropagateItemJob(propagator, item)
    {
    }
    void start() override
    {
        SyncFileItem::Status status = _item->_status;
        if (status == SyncFileItem::NoStatus) {
            if (_item->_instruction == CSYNC_INSTRUCTION_ERROR) {
                status = SyncFileItem::NormalError;
            } else {
                status = SyncFileItem::FileIgnored;
                ASSERT(_item->_instruction == CSYNC_INSTRUCTION_IGNORE);
            }
        }
        done(status, _item->_errorString);
    }
};

class OWNCLOUDSYNC_EXPORT OwncloudPropagator : public QObject
{
    Q_OBJECT
public:
    SyncJournalDb *const _journal;
    bool _finishedEmited; // used to ensure that finished is only emitted once

public:
    OwncloudPropagator(AccountPtr account, const QString &localDir,
        const QString &remoteFolder, SyncJournalDb *progressDb)
        : _localDir((localDir.endsWith(QChar('/'))) ? localDir : localDir + '/')
        , _remoteFolder((remoteFolder.endsWith(QChar('/'))) ? remoteFolder : remoteFolder + '/')
        , _journal(progressDb)
        , _finishedEmited(false)
        , _bandwidthManager(this)
        , _anotherSyncNeeded(false)
        , _chunkSize(10 * 1000 * 1000) // 10 MB, overridden in setSyncOptions
        , _account(account)
    {
        qRegisterMetaType<PropagatorJob::AbortType>("PropagatorJob::AbortType");
    }

    ~OwncloudPropagator();

    void start(const SyncFileItemVector &_syncedItems);

    const SyncOptions &syncOptions() const;
    void setSyncOptions(const SyncOptions &syncOptions);

    int _downloadLimit = 0;
    int _uploadLimit = 0;
    BandwidthManager _bandwidthManager;

    bool _abortRequested = false;

    /** The list of currently active jobs.
        This list contains the jobs that are currently using ressources and is used purely to
        know how many jobs there is currently running for the scheduler.
        Jobs add themself to the list when they do an assynchronous operation.
        Jobs can be several time on the list (example, when several chunks are uploaded in parallel)
     */
    QList<PropagateItemJob *> _activeJobList;

    /** We detected that another sync is required after this one */
    bool _anotherSyncNeeded;

    /** Per-folder quota guesses.
     *
     * This starts out empty. When an upload in a folder fails due to insufficent
     * remote quota, the quota guess is updated to be attempted_size-1 at maximum.
     *
     * Note that it will usually just an upper limit for the actual quota - but
     * since the quota on the server might change at any time it can sometimes be
     * wrong in the other direction as well.
     *
     * This allows skipping of uploads that have a very high likelihood of failure.
     */
    QHash<QString, qint64> _folderQuota;

    /* the maximum number of jobs using bandwidth (uploads or downloads, in parallel) */
    int maximumActiveTransferJob();

    /** The size to use for upload chunks.
     *
     * Will be dynamically adjusted after each chunk upload finishes
     * if Capabilities::desiredChunkUploadDuration has a target
     * chunk-upload duration set.
     */
    qint64 _chunkSize;
    qint64 smallFileSize();

    /* The maximum number of active jobs in parallel  */
    int hardMaximumActiveJob();

    /** Check whether a download would clash with an existing file
     * in filesystems that are only case-preserving.
     */
    bool localFileNameClash(const QString &relfile);

    /** Check whether a file is properly accessible for upload.
     *
     * It is possible to create files with filenames that differ
     * only by case in NTFS, but most operations such as stat and
     * open only target one of these by default.
     *
     * When that happens, we want to avoid uploading incorrect data
     * and give up on the file.
     */
    bool hasCaseClashAccessibilityProblem(const QString &relfile);

    Q_REQUIRED_RESULT QString fullLocalPath(const QString &tmp_file_name) const;
    QString localPath() const;

    /**
     * Returns the full remote path including the folder root of a
     * folder sync path.
     */
    Q_REQUIRED_RESULT QString fullRemotePath(const QString &tmp_file_name) const;
    QString remotePath() const;

    /** Creates the job for an item.
     */
    PropagateItemJob *createJob(const SyncFileItemPtr &item);

    void scheduleNextJob();
    void reportProgress(const SyncFileItem &, qint64 bytes);

    void abort()
    {
        if (_abortRequested)
            return;
        if (_rootJob) {
            // Connect to abortFinished  which signals that abort has been asynchronously finished
            connect(_rootJob.data(), &PropagateDirectory::abortFinished, this, &OwncloudPropagator::emitFinished);

            // Use Queued Connection because we're possibly already in an item's finished stack
            QMetaObject::invokeMethod(_rootJob.data(), "abort", Qt::QueuedConnection,
                                      Q_ARG(PropagatorJob::AbortType, PropagatorJob::AbortType::Asynchronous));

            // Give asynchronous abort 5000 msec to finish on its own
            QTimer::singleShot(5000, this, SLOT(abortTimeout()));
        } else {
            // No root job, call emitFinished
            emitFinished(SyncFileItem::NormalError);
        }
    }

    AccountPtr account() const;

    enum DiskSpaceResult {
        DiskSpaceOk,
        DiskSpaceFailure,
        DiskSpaceCritical
    };

    /** Checks whether there's enough disk space available to complete
     *  all jobs that are currently running.
     */
    DiskSpaceResult diskSpaceCheck() const;

    /** Handles a conflict by renaming the file 'item'.
     *
     * Sets up conflict records.
     *
     * It also creates a new upload job in composite if the item that's
     * moved away is a file and conflict uploads are requested.
     *
     * Returns true on success, false and error on error.
     */
    bool createConflict(const SyncFileItemPtr &item,
        PropagatorCompositeJob *composite, QString *error);

    // Map original path (as in the DB) to target final path
    QMap<QString, QString> _renamedDirectories;
    QString adjustRenamedPath(const QString &original) const;

    /** Update the database for an item.
     *
     * Typically after a sync operation succeeded. Updates the inode from
     * the filesystem.
     *
     * Will also trigger a Vfs::convertToPlaceholder.
     */
    Result<Vfs::ConvertToPlaceholderResult, QString> updateMetadata(const SyncFileItem &item);

    /** Update the database for an item.
     *
     * Typically after a sync operation succeeded. Updates the inode from
     * the filesystem.
     *
     * Will also trigger a Vfs::convertToPlaceholder.
     */
    static Result<Vfs::ConvertToPlaceholderResult, QString> staticUpdateMetadata(const SyncFileItem &item, const QString localDir,
                                                                                 Vfs *vfs, SyncJournalDb * const journal);

private slots:

    void abortTimeout()
    {
        // Abort synchronously and finish
        _rootJob.data()->abort(PropagatorJob::AbortType::Synchronous);
        emitFinished(SyncFileItem::NormalError);
    }

    /** Emit the finished signal and make sure it is only emitted once */
    void emitFinished(SyncFileItem::Status status)
    {
        if (!_finishedEmited)
            emit finished(status == SyncFileItem::Success);
        _finishedEmited = true;
    }

    void scheduleNextJobImpl();

signals:
    void newItem(const SyncFileItemPtr &);
    void itemCompleted(const SyncFileItemPtr &);
    void progress(const SyncFileItem &, qint64 bytes);
    void finished(bool success);

    /** Emitted when propagation has problems with a locked file. */
    void seenLockedFile(const QString &fileName);

    /** Emitted when propagation touches a file.
     *
     * Used to track our own file modifications such that notifications
     * from the file watcher about these can be ignored.
     */
    void touchedFile(const QString &fileName);

    void insufficientLocalStorage();
    void insufficientRemoteStorage();

private:
    AccountPtr _account;
    QScopedPointer<PropagateRootDirectory> _rootJob;
    SyncOptions _syncOptions;
    bool _jobScheduled = false;

    const QString _localDir; // absolute path to the local directory. ends with '/'
    const QString _remoteFolder; // remote folder, ends with '/'
};


/**
 * @brief Job that wait for all the poll jobs to be completed
 * @ingroup libsync
 */
class CleanupPollsJob : public QObject
{
    Q_OBJECT
    QVector<SyncJournalDb::PollInfo> _pollInfos;
    AccountPtr _account;
    SyncJournalDb *_journal;
    QString _localPath;
    QSharedPointer<Vfs> _vfs;

public:
    explicit CleanupPollsJob(const QVector<SyncJournalDb::PollInfo> &pollInfos, AccountPtr account, SyncJournalDb *journal, const QString &localPath,
                             const QSharedPointer<Vfs> &vfs, QObject *parent = nullptr)
        : QObject(parent)
        , _pollInfos(pollInfos)
        , _account(account)
        , _journal(journal)
        , _localPath(localPath)
        , _vfs(vfs)
    {
    }

    ~CleanupPollsJob();

    /**
     * Start the job.  After the job is completed, it will emit either finished or aborted, and it
     * will destroy itself.
     */
    void start();
signals:
    void finished();
    void aborted(const QString &error);
private slots:
    void slotPollFinished();
};
}

#endif
