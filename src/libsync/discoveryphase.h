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

#include <QObject>
#include <QElapsedTimer>
#include <QStringList>
#include <csync.h>
#include <QMap>
#include "networkjobs.h"
#include <QMutex>
#include <QWaitCondition>
#include <QLinkedList>
#include <deque>
#include "syncoptions.h"

namespace OCC {

class Account;

/**
 * The Discovery Phase was once called "update" phase in csync terms.
 * Its goal is to look at the files in one of the remote and check compared to the db
 * if the files are new, or changed.
 */

struct DiscoveryDirectoryResult
{
    QString path;
    QString msg;
    int code;
    std::deque<std::unique_ptr<csync_file_stat_t>> list;
    DiscoveryDirectoryResult()
        : code(EIO)
    {
    }
};

/**
 * @brief The DiscoverySingleDirectoryJob class
 *
 * Run in the main thread, reporting to the DiscoveryJobMainThread object
 *
 * @ingroup libsync
 */
class DiscoverySingleDirectoryJob : public QObject
{
    Q_OBJECT
public:
    explicit DiscoverySingleDirectoryJob(const AccountPtr &account, const QString &path, QObject *parent = nullptr);
    // Specify thgat this is the root and we need to check the data-fingerprint
    void setIsRootPath() { _isRootPath = true; }
    void start();
    void abort();
    std::deque<std::unique_ptr<csync_file_stat_t>> &&takeResults() { return std::move(_results); }

    // This is not actually a network job, it is just a job
signals:
    void firstDirectoryPermissions(RemotePermissions);
    void etagConcatenation(const QString &);
    void etag(const QString &);
    void finishedWithResult();
    void finishedWithError(int csyncErrnoCode, const QString &msg);
private slots:
    void directoryListingIteratedSlot(QString, const QMap<QString, QString> &);
    void lsJobFinishedWithoutErrorSlot();
    void lsJobFinishedWithErrorSlot(QNetworkReply *);

private:
    std::deque<std::unique_ptr<csync_file_stat_t>> _results;
    QString _subPath;
    QString _etagConcatenation;
    QString _firstEtag;
    AccountPtr _account;
    // The first result is for the directory itself and need to be ignored.
    // This flag is true if it was already ignored.
    bool _ignoredFirst;
    // Set to true if this is the root path and we need to check the data-fingerprint
    bool _isRootPath;
    // If this directory is an external storage (The first item has 'M' in its permission)
    bool _isExternalStorage;
    // If set, the discovery will finish with an error
    QString _error;
    QPointer<LsColJob> _lsColJob;

public:
    QByteArray _dataFingerprint;
};

// Lives in main thread. Deleted by the SyncEngine
class DiscoveryJob;
class DiscoveryMainThread : public QObject
{
    Q_OBJECT

    QPointer<DiscoveryJob> _discoveryJob;
    QPointer<DiscoverySingleDirectoryJob> _singleDirJob;
    QString _pathPrefix; // remote path
    AccountPtr _account;
    DiscoveryDirectoryResult *_currentDiscoveryDirectoryResult;
    qint64 *_currentGetSizeResult;
    bool _firstFolderProcessed;

public:
    DiscoveryMainThread(AccountPtr account)
        : QObject()
        , _account(account)
        , _currentDiscoveryDirectoryResult(nullptr)
        , _currentGetSizeResult(nullptr)
        , _firstFolderProcessed(false)
    {
    }
    void abort();

    QByteArray _dataFingerprint;


public slots:
    // From DiscoveryJob:
    void doOpendirSlot(const QString &url, DiscoveryDirectoryResult *);
    void doGetSizeSlot(const QString &path, qint64 *result);

    // From Job:
    void singleDirectoryJobResultSlot();
    void singleDirectoryJobFinishedWithErrorSlot(int csyncErrnoCode, const QString &msg);
    void singleDirectoryJobFirstDirectoryPermissionsSlot(RemotePermissions);

    void slotGetSizeFinishedWithError();
    void slotGetSizeResult(const QVariantMap &);
signals:
    void etag(const QString &);
    void etagConcatenation(const QString &);

public:
    void setupHooks(DiscoveryJob *discoveryJob, const QString &pathPrefix);
};

/**
 * @brief The DiscoveryJob class
 *
 * Lives in the other thread, deletes itself in !start()
 *
 * @ingroup libsync
 */
class DiscoveryJob : public QObject
{
    Q_OBJECT
    friend class DiscoveryMainThread;
    CSYNC *_csync_ctx;
    QElapsedTimer _lastUpdateProgressCallbackCall;

    /**
     * return true if the given path should be ignored,
     * false if the path should be synced
     */
    bool isInSelectiveSyncBlackList(const QByteArray &path) const;
    static int isInSelectiveSyncBlackListCallback(void *, const QByteArray &);
    bool checkSelectiveSyncNewObject(const QString &path, RemotePermissions rp, const bool &isDirectory);
    static int checkSelectiveSyncNewObjectCallback(void *data, const QByteArray &path, RemotePermissions rm, const bool &isDirectory);

    // Just for progress
    static void update_job_update_callback(bool local,
        const char *dirname,
        void *userdata);

    // For using QNAM to get the directory listings
    static csync_vio_handle_t *remote_vio_opendir_hook(const char *url,
        void *userdata);
    static std::unique_ptr<csync_file_stat_t> remote_vio_readdir_hook(csync_vio_handle_t *dhandle,
        void *userdata);
    static void remote_vio_closedir_hook(csync_vio_handle_t *dhandle,
        void *userdata);
    QMutex _vioMutex;
    QWaitCondition _vioWaitCondition;


public:
    explicit DiscoveryJob(CSYNC *ctx, QObject *parent = nullptr)
        : QObject(parent)
        , _csync_ctx(ctx)
    {
    }
    QStringList _selectiveSyncBlackList;
    QStringList _selectiveSyncWhiteList;
    SyncOptions _syncOptions;
    Q_INVOKABLE void start();
signals:
    void finished(int result);
    void folderDiscovered(bool local, QString folderUrl);

    // After the discovery job has been woken up again (_vioWaitCondition)
    void doOpendirSignal(QString url, DiscoveryDirectoryResult *);
    void doGetSizeSignal(const QString &path, qint64 *result);

    // A new object was discovered and was not synced because of the confirmation feature
    void newBigObject(const QString &folder, bool isExternal, bool isFolder);
};
}
