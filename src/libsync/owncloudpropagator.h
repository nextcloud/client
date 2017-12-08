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
#include <QLinkedList>
#include <QElapsedTimer>
#include <QTimer>
#include <QPointer>
#include <QIODevice>
#include <QMutex>

#include "csync_util.h"
#include "syncfileitem.h"
#include "common/syncjournaldb.h"
#include "bandwidthmanager.h"
#include "accountfwd.h"
#include "discoveryphase.h"

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
};

/*
 * Abstract class to propagate a single item
 */
class PropagateItemJob : public PropagatorJob
{
    Q_OBJECT
protected:
    void done(SyncFileItem::Status status, const QString &errorString = QString());

    bool checkForProblemsWithShared(int httpStatusCode, const QString &msg);

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

protected slots:
    void slotRestoreJobFinished(SyncFileItem::Status status);

private:
    QScopedPointer<PropagateItemJob> _restoreJob;

public:
    PropagateItemJob(OwncloudPropagator *propagator, const SyncFileItemPtr &item)
        : PropagatorJob(propagator)
        , _item(item)
    {
    }
    ~PropagateItemJob();

    bool scheduleSelfOrChild() Q_DECL_OVERRIDE
    {
        if (_state != NotYetStarted) {
            return false;
        }
        const char *instruction_str = csync_instruction_str(_item->_instruction);
        qCInfo(lcPropagator) << "Starting" << instruction_str << "propagation of" << _item->_file << "by" << this;

        _state = Running;
        QMetaObject::invokeMethod(this, "start"); // We could be in a different thread (neon jobs)
        return true;
    }

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

    virtual ~PropagatorCompositeJob()
    {
        qDeleteAll(_jobsToDo);
        qDeleteAll(_runningJobs);
    }

    void appendJob(PropagatorJob *job)
    {
        _jobsToDo.append(job);
    }
    void appendTask(const SyncFileItemPtr &item)
    {
        _tasksToDo.append(item);
    }

    virtual bool scheduleSelfOrChild() Q_DECL_OVERRIDE;
    virtual JobParallelism parallelism() Q_DECL_OVERRIDE;

    /*
     * Abort synchronously or asynchronously - some jobs
     * require to be finished without immediete abort (abort on job might
     * cause conflicts/duplicated files - owncloud/client/issues/5949)
     */
    virtual void abort(PropagatorJob::AbortType abortType) Q_DECL_OVERRIDE
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

    qint64 committedDiskSpace() const Q_DECL_OVERRIDE;

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

    explicit PropagateDirectory(OwncloudPropagator *propagator, const SyncFileItemPtr &item = SyncFileItemPtr(new SyncFileItem));

    void appendJob(PropagatorJob *job)
    {
        _subJobs.appendJob(job);
    }

    void appendTask(const SyncFileItemPtr &item)
    {
        _subJobs.appendTask(item);
    }

    virtual bool scheduleSelfOrChild() Q_DECL_OVERRIDE;
    virtual JobParallelism parallelism() Q_DECL_OVERRIDE;
    virtual void abort(PropagatorJob::AbortType abortType) Q_DECL_OVERRIDE
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


    qint64 committedDiskSpace() const Q_DECL_OVERRIDE
    {
        return _subJobs.committedDiskSpace();
    }

private slots:

    void slotFirstJobFinished(SyncFileItem::Status status);
    void slotSubJobsFinished(SyncFileItem::Status status);

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
    void start() Q_DECL_OVERRIDE
    {
        SyncFileItem::Status status = _item->_status;
        done(status == SyncFileItem::NoStatus ? SyncFileItem::FileIgnored : status, _item->_errorString);
    }
};

class OwncloudPropagator : public QObject
{
    Q_OBJECT
public:
    const QString _localDir; // absolute path to the local directory. ends with '/'
    const QString _remoteFolder; // remote folder, ends with '/'

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

    QAtomicInt _downloadLimit;
    QAtomicInt _uploadLimit;
    BandwidthManager _bandwidthManager;

    QAtomicInt _abortRequested; // boolean set by the main thread to abort.

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
    QHash<QString, quint64> _folderQuota;

    /* the maximum number of jobs using bandwidth (uploads or downloads, in parallel) */
    int maximumActiveTransferJob();

    /** The size to use for upload chunks.
     *
     * Will be dynamically adjusted after each chunk upload finishes
     * if Capabilities::desiredChunkUploadDuration has a target
     * chunk-upload duration set.
     */
    quint64 _chunkSize;
    quint64 smallFileSize();

    /* The maximum number of active jobs in parallel  */
    int hardMaximumActiveJob();

    bool isInSharedDirectory(const QString &file);

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

    /* returns the local file path for the given tmp_file_name */
    QString getFilePath(const QString &tmp_file_name) const;

    PropagateItemJob *createJob(const SyncFileItemPtr &item);
    void scheduleNextJob();
    void reportProgress(const SyncFileItem &, quint64 bytes);

    void abort()
    {
        bool alreadyAborting = _abortRequested.fetchAndStoreOrdered(true);
        if (alreadyAborting)
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

    // timeout in seconds
    static int httpTimeout();

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
    void itemCompleted(const SyncFileItemPtr &);
    void progress(const SyncFileItem &, quint64 bytes);
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
    QScopedPointer<PropagateDirectory> _rootJob;
    SyncOptions _syncOptions;
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

public:
    explicit CleanupPollsJob(const QVector<SyncJournalDb::PollInfo> &pollInfos, AccountPtr account,
        SyncJournalDb *journal, const QString &localPath, QObject *parent = 0)
        : QObject(parent)
        , _pollInfos(pollInfos)
        , _account(account)
        , _journal(journal)
        , _localPath(localPath)
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
