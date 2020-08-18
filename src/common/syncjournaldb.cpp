/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <QCryptographicHash>
#include <QFile>
#include <QLoggingCategory>
#include <QStringList>
#include <QElapsedTimer>
#include <QUrl>
#include <QDir>
#include <QStandardPaths>
#include <sqlite3.h>

#include "common/syncjournaldb.h"
#include "version.h"
#include "filesystembase.h"
#include "common/asserts.h"
#include "common/checksums.h"

#include "common/c_jhash.h"

// SQL expression to check whether path.startswith(prefix + '/')
// Note: '/' + 1 == '0'
#define IS_PREFIX_PATH_OF(prefix, path) \
    "(" path " > (" prefix "||'/') AND " path " < (" prefix "||'0'))"
#define IS_PREFIX_PATH_OR_EQUAL(prefix, path) \
    "(" path " == " prefix " OR " IS_PREFIX_PATH_OF(prefix, path) ")"

namespace OCC {

Q_LOGGING_CATEGORY(lcDb, "nextcloud.sync.database", QtInfoMsg)

#define GET_FILE_RECORD_QUERY \
        "SELECT path, inode, modtime, type, md5, fileid, remotePerm, filesize," \
        "  ignoredChildrenRemote, contentchecksumtype.name || ':' || contentChecksum, e2eMangledName, isE2eEncrypted " \
        " FROM metadata" \
        "  LEFT JOIN checksumtype as contentchecksumtype ON metadata.contentChecksumTypeId == contentchecksumtype.id"

static void fillFileRecordFromGetQuery(SyncJournalFileRecord &rec, SqlQuery &query)
{
    rec._path = query.baValue(0);
    rec._inode = query.int64Value(1);
    rec._modtime = query.int64Value(2);
    rec._type = static_cast<ItemType>(query.intValue(3));
    rec._etag = query.baValue(4);
    rec._fileId = query.baValue(5);
    rec._remotePerm = RemotePermissions(query.baValue(6).constData());
    rec._fileSize = query.int64Value(7);
    rec._serverHasIgnoredFiles = (query.intValue(8) > 0);
    rec._checksumHeader = query.baValue(9);
    rec._e2eMangledName = query.baValue(10);
    rec._isE2eEncrypted = query.intValue(11) > 0;
}

static QByteArray defaultJournalMode(const QString &dbPath)
{
#if defined(Q_OS_WIN)
    // See #2693: Some exFAT file systems seem unable to cope with the
    // WAL journaling mode. They work fine with DELETE.
    QString fileSystem = FileSystem::fileSystemForPath(dbPath);
    qCInfo(lcDb) << "Detected filesystem" << fileSystem << "for" << dbPath;
    if (fileSystem.contains("FAT")) {
        qCInfo(lcDb) << "Filesystem contains FAT - using DELETE journal mode";
        return "DELETE";
    }
#elif defined(Q_OS_MAC)
    if (dbPath.startsWith("/Volumes/")) {
        qCInfo(lcDb) << "Mounted sync dir, do not use WAL for" << dbPath;
        return "DELETE";
    }
#else
    Q_UNUSED(dbPath)
#endif
    return "WAL";
}

SyncJournalDb::SyncJournalDb(const QString &dbFilePath, QObject *parent)
    : QObject(parent)
    , _dbFile(dbFilePath)
    , _mutex(QMutex::Recursive)
    , _transaction(0)
    , _metadataTableIsEmpty(false)
{
    // Allow forcing the journal mode for debugging
    static QByteArray envJournalMode = qgetenv("OWNCLOUD_SQLITE_JOURNAL_MODE");
    _journalMode = envJournalMode;
    if (_journalMode.isEmpty()) {
        _journalMode = defaultJournalMode(_dbFile);
    }
}

QString SyncJournalDb::makeDbName(const QUrl &remoteUrl,
    const QString &remotePath,
    const QString &user)
{
    const QString dbPath = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir(dbPath).exists()) {
        QDir().mkdir(dbPath);
    }

    QString journalPath = QLatin1String("._sync_");

    QString key = QString::fromUtf8("%1@%2:%3").arg(user, remoteUrl.toString(), remotePath);

    QByteArray ba = QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Md5);
    journalPath.append(ba.left(6).toHex());
    journalPath.append(".db");

    journalPath = dbPath + QLatin1Char('/') + journalPath;

    // If the journal doesn't exist and we can't create a file
    // at that location, try again with a journal name that doesn't
    // have the ._ prefix.
    //
    // See #5633: "._*" is often forbidden on samba shared folders.

    // If it exists already, the path is clearly usable
    QFile file(QDir(dbPath).filePath(journalPath));
    if (file.exists()) {
        return journalPath;
    }

    // Try to create a file there
    if (file.open(QIODevice::ReadWrite)) {
        // Ok, all good.
        file.close();
        file.remove();
        return journalPath;
    }

    // Can we create it if we drop the underscore?
    QString alternateJournalPath = journalPath.mid(2).prepend(".");
    QFile file2(QDir(dbPath).filePath(alternateJournalPath));
    if (file2.open(QIODevice::ReadWrite)) {
        // The alternative worked, use it
        qCInfo(lcDb) << "Using alternate database path" << alternateJournalPath;
        file2.close();
        file2.remove();
        return alternateJournalPath;
    }

    // Neither worked, just keep the original and throw errors later
    qCWarning(lcDb) << "Could not find a writable database path" << file.fileName();
    return journalPath;
}

bool SyncJournalDb::maybeMigrateDb(const QString &localPath, const QString &absoluteJournalPath)
{
    const QString oldDbName = localPath + QLatin1String(".csync_journal.db");
    if (!FileSystem::fileExists(oldDbName)) {
        return true;
    }
    const QString oldDbNameShm = oldDbName + "-shm";
    const QString oldDbNameWal = oldDbName + "-wal";

    const QString newDbName = absoluteJournalPath;
    const QString newDbNameShm = newDbName + "-shm";
    const QString newDbNameWal = newDbName + "-wal";

    // Whenever there is an old db file, migrate it to the new db path.
    // This is done to make switching from older versions to newer versions
    // work correctly even if the user had previously used a new version
    // and therefore already has an (outdated) new-style db file.
    QString error;

    if (FileSystem::fileExists(newDbName)) {
        if (!FileSystem::remove(newDbName, &error)) {
            qCWarning(lcDb) << "Database migration: Could not remove db file" << newDbName
                            << "due to" << error;
            return false;
        }
    }
    if (FileSystem::fileExists(newDbNameWal)) {
        if (!FileSystem::remove(newDbNameWal, &error)) {
            qCWarning(lcDb) << "Database migration: Could not remove db WAL file" << newDbNameWal
                            << "due to" << error;
            return false;
        }
    }
    if (FileSystem::fileExists(newDbNameShm)) {
        if (!FileSystem::remove(newDbNameShm, &error)) {
            qCWarning(lcDb) << "Database migration: Could not remove db SHM file" << newDbNameShm
                            << "due to" << error;
            return false;
        }
    }

    if (!FileSystem::rename(oldDbName, newDbName, &error)) {
        qCWarning(lcDb) << "Database migration: could not rename " << oldDbName
                        << "to" << newDbName << ":" << error;
        return false;
    }
    if (!FileSystem::rename(oldDbNameWal, newDbNameWal, &error)) {
        qCWarning(lcDb) << "Database migration: could not rename " << oldDbNameWal
                        << "to" << newDbNameWal << ":" << error;
        return false;
    }
    if (!FileSystem::rename(oldDbNameShm, newDbNameShm, &error)) {
        qCWarning(lcDb) << "Database migration: could not rename " << oldDbNameShm
                        << "to" << newDbNameShm << ":" << error;
        return false;
    }

    qCInfo(lcDb) << "Journal successfully migrated from" << oldDbName << "to" << newDbName;
    return true;
}

bool SyncJournalDb::exists()
{
    QMutexLocker locker(&_mutex);
    return (!_dbFile.isEmpty() && QFile::exists(_dbFile));
}

QString SyncJournalDb::databaseFilePath() const
{
    return _dbFile;
}

// Note that this does not change the size of the -wal file, but it is supposed to make
// the normal .db faster since the changes from the wal will be incorporated into it.
// Then the next sync (and the SocketAPI) will have a faster access.
void SyncJournalDb::walCheckpoint()
{
    QElapsedTimer t;
    t.start();
    SqlQuery pragma1(_db);
    pragma1.prepare("PRAGMA wal_checkpoint(FULL);");
    if (pragma1.exec()) {
        qCDebug(lcDb) << "took" << t.elapsed() << "msec";
    }
}

void SyncJournalDb::startTransaction()
{
    if (_transaction == 0) {
        if (!_db.transaction()) {
            qCWarning(lcDb) << "ERROR starting transaction: " << _db.error();
            return;
        }
        _transaction = 1;
    } else {
        qCDebug(lcDb) << "Database Transaction is running, not starting another one!";
    }
}

void SyncJournalDb::commitTransaction()
{
    if (_transaction == 1) {
        if (!_db.commit()) {
            qCWarning(lcDb) << "ERROR committing to the database: " << _db.error();
            return;
        }
        _transaction = 0;
    } else {
        qCDebug(lcDb) << "No database Transaction to commit";
    }
}

bool SyncJournalDb::sqlFail(const QString &log, const SqlQuery &query)
{
    commitTransaction();
    qCWarning(lcDb) << "SQL Error" << log << query.error();
    _db.close();
    ASSERT(false);
    return false;
}

bool SyncJournalDb::checkConnect()
{
    if (_db.isOpen()) {
        // Unfortunately the sqlite isOpen check can return true even when the underlying storage
        // has become unavailable - and then some operations may cause crashes. See #6049
        if (!QFile::exists(_dbFile)) {
            qCWarning(lcDb) << "Database open, but file " + _dbFile + " does not exist";
            close();
            return false;
        }
        return true;
    }

    if (_dbFile.isEmpty()) {
        qCWarning(lcDb) << "Database filename" + _dbFile + " is empty";
        return false;
    }

    // The database file is created by this call (SQLITE_OPEN_CREATE)
    if (!_db.openOrCreateReadWrite(_dbFile)) {
        QString error = _db.error();
        qCWarning(lcDb) << "Error opening the db: " << error;
        return false;
    }

    if (!QFile::exists(_dbFile)) {
        qCWarning(lcDb) << "Database file" + _dbFile + " does not exist";
        return false;
    }

    SqlQuery pragma1(_db);
    pragma1.prepare("SELECT sqlite_version();");
    if (!pragma1.exec()) {
        return sqlFail("SELECT sqlite_version()", pragma1);
    } else {
        pragma1.next();
        qCInfo(lcDb) << "sqlite3 version" << pragma1.stringValue(0);
    }

    pragma1.prepare("PRAGMA journal_mode=" + _journalMode + ";");
    if (!pragma1.exec()) {
        return sqlFail("Set PRAGMA journal_mode", pragma1);
    } else {
        pragma1.next();
        qCInfo(lcDb) << "sqlite3 journal_mode=" << pragma1.stringValue(0);
    }

    // For debugging purposes, allow temp_store to be set
    static QByteArray env_temp_store = qgetenv("OWNCLOUD_SQLITE_TEMP_STORE");
    if (!env_temp_store.isEmpty()) {
        pragma1.prepare("PRAGMA temp_store = " + env_temp_store + ";");
        if (!pragma1.exec()) {
            return sqlFail("Set PRAGMA temp_store", pragma1);
        }
        qCInfo(lcDb) << "sqlite3 with temp_store =" << env_temp_store;
    }

    pragma1.prepare("PRAGMA synchronous = 1;");
    if (!pragma1.exec()) {
        return sqlFail("Set PRAGMA synchronous", pragma1);
    }
    pragma1.prepare("PRAGMA case_sensitive_like = ON;");
    if (!pragma1.exec()) {
        return sqlFail("Set PRAGMA case_sensitivity", pragma1);
    }

    /* Because insert is so slow, we do everything in a transaction, and only need one call to commit */
    startTransaction();

    SqlQuery createQuery(_db);
    createQuery.prepare("CREATE TABLE IF NOT EXISTS metadata("
                        "phash INTEGER(8),"
                        "pathlen INTEGER,"
                        "path VARCHAR(4096),"
                        "inode INTEGER,"
                        "uid INTEGER,"
                        "gid INTEGER,"
                        "mode INTEGER,"
                        "modtime INTEGER(8),"
                        "type INTEGER,"
                        "md5 VARCHAR(32)," /* This is the etag.  Called md5 for compatibility */
                        // updateDatabaseStructure() will add
                        // fileid
                        // remotePerm
                        // filesize
                        // ignoredChildrenRemote
                        // contentChecksum
                        // contentChecksumTypeId
                        "PRIMARY KEY(phash)"
                        ");");

#ifndef SQLITE_IOERR_SHMMAP
// Requires sqlite >= 3.7.7 but old CentOS6 has sqlite-3.6.20
// Definition taken from https://sqlite.org/c3ref/c_abort_rollback.html
#define SQLITE_IOERR_SHMMAP            (SQLITE_IOERR | (21<<8))
#endif

    if (!createQuery.exec()) {
        // In certain situations the io error can be avoided by switching
        // to the DELETE journal mode, see #5723
        if (_journalMode != "DELETE"
            && createQuery.errorId() == SQLITE_IOERR
            && sqlite3_extended_errcode(_db.sqliteDb()) == SQLITE_IOERR_SHMMAP) {
            qCWarning(lcDb) << "IO error SHMMAP on table creation, attempting with DELETE journal mode";

            _journalMode = "DELETE";
            createQuery.finish();
            pragma1.finish();
            commitTransaction();
            _db.close();
            return checkConnect();
        }

        return sqlFail("Create table metadata", createQuery);
    }

    createQuery.prepare("CREATE TABLE IF NOT EXISTS downloadinfo("
                        "path VARCHAR(4096),"
                        "tmpfile VARCHAR(4096),"
                        "etag VARCHAR(32),"
                        "errorcount INTEGER,"
                        "PRIMARY KEY(path)"
                        ");");

    if (!createQuery.exec()) {
        return sqlFail("Create table downloadinfo", createQuery);
    }

    createQuery.prepare("CREATE TABLE IF NOT EXISTS uploadinfo("
                        "path VARCHAR(4096),"
                        "chunk INTEGER,"
                        "transferid INTEGER,"
                        "errorcount INTEGER,"
                        "size INTEGER(8),"
                        "modtime INTEGER(8),"
                        "contentChecksum TEXT,"
                        "PRIMARY KEY(path)"
                        ");");

    if (!createQuery.exec()) {
        return sqlFail("Create table uploadinfo", createQuery);
    }

    // create the blacklist table.
    createQuery.prepare("CREATE TABLE IF NOT EXISTS blacklist ("
                        "path VARCHAR(4096),"
                        "lastTryEtag VARCHAR[32],"
                        "lastTryModtime INTEGER[8],"
                        "retrycount INTEGER,"
                        "errorstring VARCHAR[4096],"
                        "PRIMARY KEY(path)"
                        ");");

    if (!createQuery.exec()) {
        return sqlFail("Create table blacklist", createQuery);
    }

    createQuery.prepare("CREATE TABLE IF NOT EXISTS poll("
                        "path VARCHAR(4096),"
                        "modtime INTEGER(8),"
                        "pollpath VARCHAR(4096));");
    if (!createQuery.exec()) {
        return sqlFail("Create table poll", createQuery);
    }

    // create the selectivesync table.
    createQuery.prepare("CREATE TABLE IF NOT EXISTS selectivesync ("
                        "path VARCHAR(4096),"
                        "type INTEGER"
                        ");");

    if (!createQuery.exec()) {
        return sqlFail("Create table selectivesync", createQuery);
    }

    // create the checksumtype table.
    createQuery.prepare("CREATE TABLE IF NOT EXISTS checksumtype("
                        "id INTEGER PRIMARY KEY,"
                        "name TEXT UNIQUE"
                        ");");
    if (!createQuery.exec()) {
        return sqlFail("Create table version", createQuery);
    }

    // create the datafingerprint table.
    createQuery.prepare("CREATE TABLE IF NOT EXISTS datafingerprint("
                        "fingerprint TEXT UNIQUE"
                        ");");
    if (!createQuery.exec()) {
        return sqlFail("Create table datafingerprint", createQuery);
    }

    // create the conflicts table.
    createQuery.prepare("CREATE TABLE IF NOT EXISTS conflicts("
                        "path TEXT PRIMARY KEY,"
                        "baseFileId TEXT,"
                        "baseEtag TEXT,"
                        "baseModtime INTEGER"
                        ");");
    if (!createQuery.exec()) {
        return sqlFail("Create table conflicts", createQuery);
    }

    createQuery.prepare("CREATE TABLE IF NOT EXISTS version("
                        "major INTEGER(8),"
                        "minor INTEGER(8),"
                        "patch INTEGER(8),"
                        "custom VARCHAR(256)"
                        ");");
    if (!createQuery.exec()) {
        return sqlFail("Create table version", createQuery);
    }

    bool forceRemoteDiscovery = false;

    SqlQuery versionQuery("SELECT major, minor, patch FROM version;", _db);
    if (!versionQuery.next()) {
        // If there was no entry in the table, it means we are likely upgrading from 1.5
        qCInfo(lcDb) << "possibleUpgradeFromMirall_1_5 detected!";
        forceRemoteDiscovery = true;

        createQuery.prepare("INSERT INTO version VALUES (?1, ?2, ?3, ?4);");
        createQuery.bindValue(1, MIRALL_VERSION_MAJOR);
        createQuery.bindValue(2, MIRALL_VERSION_MINOR);
        createQuery.bindValue(3, MIRALL_VERSION_PATCH);
        createQuery.bindValue(4, MIRALL_VERSION_BUILD);
        if (!createQuery.exec()) {
            return sqlFail("Update version", createQuery);
        }

    } else {
        int major = versionQuery.intValue(0);
        int minor = versionQuery.intValue(1);
        int patch = versionQuery.intValue(2);

        if (major == 1 && minor == 8 && (patch == 0 || patch == 1)) {
            qCInfo(lcDb) << "possibleUpgradeFromMirall_1_8_0_or_1 detected!";
            forceRemoteDiscovery = true;
        }

        // There was a bug in versions <2.3.0 that could lead to stale
        // local files and a remote discovery will fix them.
        // See #5190 #5242.
        if (major == 2 && minor < 3) {
            qCInfo(lcDb) << "upgrade form client < 2.3.0 detected! forcing remote discovery";
            forceRemoteDiscovery = true;
        }

        // Not comparing the BUILD id here, correct?
        if (!(major == MIRALL_VERSION_MAJOR && minor == MIRALL_VERSION_MINOR && patch == MIRALL_VERSION_PATCH)) {
            createQuery.prepare("UPDATE version SET major=?1, minor=?2, patch =?3, custom=?4 "
                                "WHERE major=?5 AND minor=?6 AND patch=?7;");
            createQuery.bindValue(1, MIRALL_VERSION_MAJOR);
            createQuery.bindValue(2, MIRALL_VERSION_MINOR);
            createQuery.bindValue(3, MIRALL_VERSION_PATCH);
            createQuery.bindValue(4, MIRALL_VERSION_BUILD);
            createQuery.bindValue(5, major);
            createQuery.bindValue(6, minor);
            createQuery.bindValue(7, patch);
            if (!createQuery.exec()) {
                return sqlFail("Update version", createQuery);
            }
        }
    }

    commitInternal("checkConnect");

    bool rc = updateDatabaseStructure();
    if (!rc) {
        qCWarning(lcDb) << "Failed to update the database structure!";
    }

    /*
     * If we are upgrading from a client version older than 1.5,
     * we cannot read from the database because we need to fetch the files id and etags.
     *
     *  If 1.8.0 caused missing data in the local tree, so we also don't read from DB
     *  to get back the files that were gone.
     *  In 1.8.1 we had a fix to re-get the data, but this one here is better
     */
    if (forceRemoteDiscovery) {
        forceRemoteDiscoveryNextSyncLocked();
    }

    if (!_deleteDownloadInfoQuery.initOrReset("DELETE FROM downloadinfo WHERE path=?1", _db)) {
        return sqlFail("prepare _deleteDownloadInfoQuery", _deleteDownloadInfoQuery);
    }

    if (!_deleteUploadInfoQuery.initOrReset("DELETE FROM uploadinfo WHERE path=?1", _db)) {
        return sqlFail("prepare _deleteUploadInfoQuery", _deleteUploadInfoQuery);
    }

    QByteArray sql("SELECT lastTryEtag, lastTryModtime, retrycount, errorstring, lastTryTime, ignoreDuration, renameTarget, errorCategory "
                   "FROM blacklist WHERE path=?1");
    if (Utility::fsCasePreserving()) {
        // if the file system is case preserving we have to check the blacklist
        // case insensitively
        sql += " COLLATE NOCASE";
    }
    if (!_getErrorBlacklistQuery.initOrReset(sql, _db)) {
        return sqlFail("prepare _getErrorBlacklistQuery", _getErrorBlacklistQuery);
    }

    // don't start a new transaction now
    commitInternal(QString("checkConnect End"), false);

    // This avoid reading from the DB if we already know it is empty
    // thereby speeding up the initial discovery significantly.
    _metadataTableIsEmpty = (getFileRecordCount() == 0);

    // Hide 'em all!
    FileSystem::setFileHidden(databaseFilePath(), true);
    FileSystem::setFileHidden(databaseFilePath() + "-wal", true);
    FileSystem::setFileHidden(databaseFilePath() + "-shm", true);
    FileSystem::setFileHidden(databaseFilePath() + "-journal", true);

    return rc;
}

void SyncJournalDb::close()
{
    QMutexLocker locker(&_mutex);
    qCInfo(lcDb) << "Closing DB" << _dbFile;

    commitTransaction();

    _db.close();
    clearEtagStorageFilter();
    _metadataTableIsEmpty = false;
}


bool SyncJournalDb::updateDatabaseStructure()
{
    if (!updateMetadataTableStructure())
        return false;
    if (!updateErrorBlacklistTableStructure())
        return false;
    return true;
}

bool SyncJournalDb::updateMetadataTableStructure()
{

    auto columns = tableColumns("metadata");
    bool re = true;

    // check if the file_id column is there and create it if not
    if (!checkConnect()) {
        return false;
    }

    if (columns.indexOf("fileid") == -1) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE metadata ADD COLUMN fileid VARCHAR(128);");
        if (!query.exec()) {
            sqlFail("updateMetadataTableStructure: Add column fileid", query);
            re = false;
        }

        query.prepare("CREATE INDEX metadata_file_id ON metadata(fileid);");
        if (!query.exec()) {
            sqlFail("updateMetadataTableStructure: create index fileid", query);
            re = false;
        }
        commitInternal("update database structure: add fileid col");
    }
    if (columns.indexOf("remotePerm") == -1) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE metadata ADD COLUMN remotePerm VARCHAR(128);");
        if (!query.exec()) {
            sqlFail("updateMetadataTableStructure: add column remotePerm", query);
            re = false;
        }
        commitInternal("update database structure (remotePerm)");
    }
    if (columns.indexOf("filesize") == -1) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE metadata ADD COLUMN filesize BIGINT;");
        if (!query.exec()) {
            sqlFail("updateDatabaseStructure: add column filesize", query);
            re = false;
        }
        commitInternal("update database structure: add filesize col");
    }

    if (true) {
        SqlQuery query(_db);
        query.prepare("CREATE INDEX IF NOT EXISTS metadata_inode ON metadata(inode);");
        if (!query.exec()) {
            sqlFail("updateMetadataTableStructure: create index inode", query);
            re = false;
        }
        commitInternal("update database structure: add inode index");
    }

    if (true) {
        SqlQuery query(_db);
        query.prepare("CREATE INDEX IF NOT EXISTS metadata_path ON metadata(path);");
        if (!query.exec()) {
            sqlFail("updateMetadataTableStructure: create index path", query);
            re = false;
        }
        commitInternal("update database structure: add path index");
    }

    if (columns.indexOf("ignoredChildrenRemote") == -1) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE metadata ADD COLUMN ignoredChildrenRemote INT;");
        if (!query.exec()) {
            sqlFail("updateMetadataTableStructure: add ignoredChildrenRemote column", query);
            re = false;
        }
        commitInternal("update database structure: add ignoredChildrenRemote col");
    }

    if (columns.indexOf("contentChecksum") == -1) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE metadata ADD COLUMN contentChecksum TEXT;");
        if (!query.exec()) {
            sqlFail("updateMetadataTableStructure: add contentChecksum column", query);
            re = false;
        }
        commitInternal("update database structure: add contentChecksum col");
    }
    if (columns.indexOf("contentChecksumTypeId") == -1) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE metadata ADD COLUMN contentChecksumTypeId INTEGER;");
        if (!query.exec()) {
            sqlFail("updateMetadataTableStructure: add contentChecksumTypeId column", query);
            re = false;
        }
        commitInternal("update database structure: add contentChecksumTypeId col");
    }

    if (!columns.contains("e2eMangledName")) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE metadata ADD COLUMN e2eMangledName TEXT;");
        if (!query.exec()) {
            sqlFail("updateMetadataTableStructure: add e2eMangledName column", query);
            re = false;
        }
        commitInternal("update database structure: add e2eMangledName col");
    }

    if (!columns.contains("isE2eEncrypted")) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE metadata ADD COLUMN isE2eEncrypted INTEGER;");
        if (!query.exec()) {
            sqlFail("updateMetadataTableStructure: add isE2eEncrypted column", query);
            re = false;
        }
        commitInternal("update database structure: add isE2eEncrypted col");
    }

    if (!tableColumns("uploadinfo").contains("contentChecksum")) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE uploadinfo ADD COLUMN contentChecksum TEXT;");
        if (!query.exec()) {
            sqlFail("updateMetadataTableStructure: add contentChecksum column", query);
            re = false;
        }
        commitInternal("update database structure: add contentChecksum col for uploadinfo");
    }


    return re;
}

bool SyncJournalDb::updateErrorBlacklistTableStructure()
{
    auto columns = tableColumns("blacklist");
    bool re = true;

    // check if the file_id column is there and create it if not
    if (!checkConnect()) {
        return false;
    }

    if (columns.indexOf("lastTryTime") == -1) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE blacklist ADD COLUMN lastTryTime INTEGER(8);");
        if (!query.exec()) {
            sqlFail("updateBlacklistTableStructure: Add lastTryTime fileid", query);
            re = false;
        }
        query.prepare("ALTER TABLE blacklist ADD COLUMN ignoreDuration INTEGER(8);");
        if (!query.exec()) {
            sqlFail("updateBlacklistTableStructure: Add ignoreDuration fileid", query);
            re = false;
        }
        commitInternal("update database structure: add lastTryTime, ignoreDuration cols");
    }
    if (columns.indexOf("renameTarget") == -1) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE blacklist ADD COLUMN renameTarget VARCHAR(4096);");
        if (!query.exec()) {
            sqlFail("updateBlacklistTableStructure: Add renameTarget", query);
            re = false;
        }
        commitInternal("update database structure: add renameTarget col");
    }

    if (columns.indexOf("errorCategory") == -1) {
        SqlQuery query(_db);
        query.prepare("ALTER TABLE blacklist ADD COLUMN errorCategory INTEGER(8);");
        if (!query.exec()) {
            sqlFail("updateBlacklistTableStructure: Add errorCategory", query);
            re = false;
        }
        commitInternal("update database structure: add errorCategory col");
    }

    SqlQuery query(_db);
    query.prepare("CREATE INDEX IF NOT EXISTS blacklist_index ON blacklist(path collate nocase);");
    if (!query.exec()) {
        sqlFail("updateErrorBlacklistTableStructure: create index blacklit", query);
        re = false;
    }

    return re;
}

QVector<QByteArray> SyncJournalDb::tableColumns(const QByteArray &table)
{
    QVector<QByteArray> columns;
    if (!checkConnect()) {
        return columns;
    }
    SqlQuery query("PRAGMA table_info('" + table + "');", _db);
    if (!query.exec()) {
        return columns;
    }
    while (query.next()) {
        columns.append(query.baValue(1));
    }
    qCDebug(lcDb) << "Columns in the current journal: " << columns;
    return columns;
}

qint64 SyncJournalDb::getPHash(const QByteArray &file)
{
    int64_t h = 0;

    if (file.isEmpty()) {
        return -1;
    }

    int len = file.length();

    h = c_jhash64((uint8_t *)file.data(), len, 0);
    return h;
}

bool SyncJournalDb::setFileRecord(const SyncJournalFileRecord &_record)
{
    SyncJournalFileRecord record = _record;
    QMutexLocker locker(&_mutex);

    if (!_etagStorageFilter.isEmpty()) {
        // If we are a directory that should not be read from db next time, don't write the etag
        QByteArray prefix = record._path + "/";
        foreach (const QByteArray &it, _etagStorageFilter) {
            if (it.startsWith(prefix)) {
                qCInfo(lcDb) << "Filtered writing the etag of" << prefix << "because it is a prefix of" << it;
                record._etag = "_invalid_";
                break;
            }
        }
    }

    qCInfo(lcDb) << "Updating file record for path:" << record._path << "inode:" << record._inode
                 << "modtime:" << record._modtime << "type:" << record._type
                 << "etag:" << record._etag << "fileId:" << record._fileId << "remotePerm:" << record._remotePerm.toString()
                 << "fileSize:" << record._fileSize << "checksum:" << record._checksumHeader
                 << "e2eMangledName:" << record._e2eMangledName << "isE2eEncrypted:" << record._isE2eEncrypted;

    qlonglong phash = getPHash(record._path);
    if (checkConnect()) {
        int plen = record._path.length();

        QByteArray etag(record._etag);
        if (etag.isEmpty())
            etag = "";
        QByteArray fileId(record._fileId);
        if (fileId.isEmpty())
            fileId = "";
        QByteArray remotePerm = record._remotePerm.toString();
        QByteArray checksumType, checksum;
        parseChecksumHeader(record._checksumHeader, &checksumType, &checksum);
        int contentChecksumTypeId = mapChecksumType(checksumType);

        if (!_setFileRecordQuery.initOrReset(QByteArrayLiteral(
            "INSERT OR REPLACE INTO metadata "
            "(phash, pathlen, path, inode, uid, gid, mode, modtime, type, md5, fileid, remotePerm, filesize, ignoredChildrenRemote, contentChecksum, contentChecksumTypeId, e2eMangledName, isE2eEncrypted) "
            "VALUES (?1 , ?2, ?3 , ?4 , ?5 , ?6 , ?7,  ?8 , ?9 , ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18);"), _db)) {
            return false;
        }

        _setFileRecordQuery.bindValue(1, phash);
        _setFileRecordQuery.bindValue(2, plen);
        _setFileRecordQuery.bindValue(3, record._path);
        _setFileRecordQuery.bindValue(4, record._inode);
        _setFileRecordQuery.bindValue(5, 0); // uid Not used
        _setFileRecordQuery.bindValue(6, 0); // gid Not used
        _setFileRecordQuery.bindValue(7, 0); // mode Not used
        _setFileRecordQuery.bindValue(8, record._modtime);
        _setFileRecordQuery.bindValue(9, record._type);
        _setFileRecordQuery.bindValue(10, etag);
        _setFileRecordQuery.bindValue(11, fileId);
        _setFileRecordQuery.bindValue(12, remotePerm);
        _setFileRecordQuery.bindValue(13, record._fileSize);
        _setFileRecordQuery.bindValue(14, record._serverHasIgnoredFiles ? 1 : 0);
        _setFileRecordQuery.bindValue(15, checksum);
        _setFileRecordQuery.bindValue(16, contentChecksumTypeId);
        _setFileRecordQuery.bindValue(17, record._e2eMangledName);
        _setFileRecordQuery.bindValue(18, record._isE2eEncrypted);

        if (!_setFileRecordQuery.exec()) {
            return false;
        }

        // Can't be true anymore.
        _metadataTableIsEmpty = false;

        return true;
    } else {
        qCWarning(lcDb) << "Failed to connect database.";
        return false; // checkConnect failed.
    }
}

bool SyncJournalDb::deleteFileRecord(const QString &filename, bool recursively)
{
    QMutexLocker locker(&_mutex);

    if (checkConnect()) {
        // if (!recursively) {
        // always delete the actual file.

        if (!_deleteFileRecordPhash.initOrReset(QByteArrayLiteral("DELETE FROM metadata WHERE phash=?1"), _db))
            return false;

        qlonglong phash = getPHash(filename.toUtf8());
        _deleteFileRecordPhash.bindValue(1, phash);

        if (!_deleteFileRecordPhash.exec())
            return false;

        if (recursively) {
            if (!_deleteFileRecordRecursively.initOrReset(QByteArrayLiteral("DELETE FROM metadata WHERE " IS_PREFIX_PATH_OF("?1", "path")), _db))
                return false;
            _deleteFileRecordRecursively.bindValue(1, filename);
            if (!_deleteFileRecordRecursively.exec()) {
                return false;
            }
        }
        return true;
    } else {
        qCWarning(lcDb) << "Failed to connect database.";
        return false; // checkConnect failed.
    }
}


bool SyncJournalDb::getFileRecord(const QByteArray &filename, SyncJournalFileRecord *rec)
{
    QMutexLocker locker(&_mutex);

    // Reset the output var in case the caller is reusing it.
    Q_ASSERT(rec);
    rec->_path.clear();
    Q_ASSERT(!rec->isValid());

    if (_metadataTableIsEmpty)
        return true; // no error, yet nothing found (rec->isValid() == false)

    if (!checkConnect())
        return false;

    if (!filename.isEmpty()) {
        if (!_getFileRecordQuery.initOrReset(QByteArrayLiteral(GET_FILE_RECORD_QUERY " WHERE phash=?1"), _db))
            return false;

        _getFileRecordQuery.bindValue(1, getPHash(filename));

        if (!_getFileRecordQuery.exec()) {
            close();
            return false;
        }

        if (_getFileRecordQuery.next()) {
            fillFileRecordFromGetQuery(*rec, _getFileRecordQuery);
        } else {
            int errId = _getFileRecordQuery.errorId();
            if (errId != SQLITE_DONE) { // only do this if the problem is different from SQLITE_DONE
                QString err = _getFileRecordQuery.error();
                qCWarning(lcDb) << "No journal entry found for " << filename << "Error: " << err;
                close();
            }
        }
    }
    return true;
}

bool SyncJournalDb::getFileRecordByE2eMangledName(const QString &mangledName, SyncJournalFileRecord *rec)
{
    QMutexLocker locker(&_mutex);

    // Reset the output var in case the caller is reusing it.
    Q_ASSERT(rec);
    rec->_path.clear();
    Q_ASSERT(!rec->isValid());

    if (_metadataTableIsEmpty) {
        return true; // no error, yet nothing found (rec->isValid() == false)
    }

    if (!checkConnect()) {
        return false;
    }

    if (!mangledName.isEmpty()) {
        if (!_getFileRecordQueryByMangledName.initOrReset(QByteArrayLiteral(GET_FILE_RECORD_QUERY " WHERE e2eMangledName=?1"), _db)) {
            return false;
        }

        _getFileRecordQueryByMangledName.bindValue(1, mangledName);

        if (!_getFileRecordQueryByMangledName.exec()) {
            close();
            return false;
        }

        if (_getFileRecordQueryByMangledName.next()) {
            fillFileRecordFromGetQuery(*rec, _getFileRecordQueryByMangledName);
        } else {
            int errId = _getFileRecordQueryByMangledName.errorId();
            if (errId != SQLITE_DONE) { // only do this if the problem is different from SQLITE_DONE
                QString err = _getFileRecordQueryByMangledName.error();
                qCWarning(lcDb) << "No journal entry found for mangled name" << mangledName << "Error: " << err;
                close();
            }
        }
    }
    return true;
}

bool SyncJournalDb::getFileRecordByInode(quint64 inode, SyncJournalFileRecord *rec)
{
    QMutexLocker locker(&_mutex);

    // Reset the output var in case the caller is reusing it.
    Q_ASSERT(rec);
    rec->_path.clear();
    Q_ASSERT(!rec->isValid());

    if (!inode || _metadataTableIsEmpty)
        return true; // no error, yet nothing found (rec->isValid() == false)

    if (!checkConnect())
        return false;

    if (!_getFileRecordQueryByInode.initOrReset(QByteArrayLiteral(GET_FILE_RECORD_QUERY " WHERE inode=?1"), _db))
        return false;

    _getFileRecordQueryByInode.bindValue(1, inode);

    if (!_getFileRecordQueryByInode.exec())
        return false;

    if (_getFileRecordQueryByInode.next())
        fillFileRecordFromGetQuery(*rec, _getFileRecordQueryByInode);

    return true;
}

bool SyncJournalDb::getFileRecordsByFileId(const QByteArray &fileId, const std::function<void(const SyncJournalFileRecord &)> &rowCallback)
{
    QMutexLocker locker(&_mutex);

    if (fileId.isEmpty() || _metadataTableIsEmpty)
        return true; // no error, yet nothing found (rec->isValid() == false)

    if (!checkConnect())
        return false;

    if (!_getFileRecordQueryByFileId.initOrReset(QByteArrayLiteral(GET_FILE_RECORD_QUERY " WHERE fileid=?1"), _db))
        return false;

    _getFileRecordQueryByFileId.bindValue(1, fileId);

    if (!_getFileRecordQueryByFileId.exec())
        return false;

    while (_getFileRecordQueryByFileId.next()) {
        SyncJournalFileRecord rec;
        fillFileRecordFromGetQuery(rec, _getFileRecordQueryByFileId);
        rowCallback(rec);
    }

    return true;
}

bool SyncJournalDb::getFilesBelowPath(const QByteArray &path, const std::function<void(const SyncJournalFileRecord&)> &rowCallback)
{
    QMutexLocker locker(&_mutex);

    if (_metadataTableIsEmpty)
        return true; // no error, yet nothing found

    if (!checkConnect())
        return false;

    SqlQuery *query = nullptr;

    if(path.isEmpty()) {
        // Since the path column doesn't store the starting /, the getFilesBelowPathQuery
        // can't be used for the root path "". It would scan for (path > '/' and path < '0')
        // and find nothing. So, unfortunately, we have to use a different query for
        // retrieving the whole tree.

        if (!_getAllFilesQuery.initOrReset(QByteArrayLiteral( GET_FILE_RECORD_QUERY " ORDER BY path||'/' ASC"), _db))
            return false;
        query = &_getAllFilesQuery;
    } else {
        // This query is used to skip discovery and fill the tree from the
        // database instead
        if (!_getFilesBelowPathQuery.initOrReset(QByteArrayLiteral(
                GET_FILE_RECORD_QUERY
                " WHERE " IS_PREFIX_PATH_OF("?1", "path")
                " OR " IS_PREFIX_PATH_OF("?1", "e2eMangledName")
                // We want to ensure that the contents of a directory are sorted
                // directly behind the directory itself. Without this ORDER BY
                // an ordering like foo, foo-2, foo/file would be returned.
                // With the trailing /, we get foo-2, foo, foo/file. This property
                // is used in fill_tree_from_db().
                " ORDER BY path||'/' ASC"), _db)) {
            return false;
        }
        query = &_getFilesBelowPathQuery;
        query->bindValue(1, path);
    }

    if (!query->exec()) {
        return false;
    }

    while (query->next()) {
        SyncJournalFileRecord rec;
        fillFileRecordFromGetQuery(rec, *query);
        rowCallback(rec);
    }

    return true;
}

bool SyncJournalDb::postSyncCleanup(const QSet<QString> &filepathsToKeep,
    const QSet<QString> &prefixesToKeep)
{
    QMutexLocker locker(&_mutex);

    if (!checkConnect()) {
        return false;
    }

    SqlQuery query(_db);
    query.prepare("SELECT phash, path, e2eMangledName FROM metadata order by path");

    if (!query.exec()) {
        return false;
    }

    QByteArrayList superfluousItems;

    while (query.next()) {
        const auto file = QString(query.baValue(1));
        const auto mangledPath = QString(query.baValue(2));
        bool keep = filepathsToKeep.contains(file) || filepathsToKeep.contains(mangledPath);
        if (!keep) {
            foreach (const QString &prefix, prefixesToKeep) {
                if (file.startsWith(prefix) || mangledPath.startsWith(prefix)) {
                    keep = true;
                    break;
                }
            }
        }
        if (!keep) {
            superfluousItems.append(query.baValue(0));
        }
    }

    if (superfluousItems.count()) {
        QByteArray sql = "DELETE FROM metadata WHERE phash in (" + superfluousItems.join(",") + ")";
        qCInfo(lcDb) << "Sync Journal cleanup for" << superfluousItems;
        SqlQuery delQuery(_db);
        delQuery.prepare(sql);
        if (!delQuery.exec()) {
            return false;
        }
    }

    // Incorporate results back into main DB
    walCheckpoint();

    return true;
}

int SyncJournalDb::getFileRecordCount()
{
    QMutexLocker locker(&_mutex);

    SqlQuery query(_db);
    query.prepare("SELECT COUNT(*) FROM metadata");

    if (!query.exec()) {
        return -1;
    }

    if (query.next()) {
        int count = query.intValue(0);
        return count;
    }

    return -1;
}

bool SyncJournalDb::updateFileRecordChecksum(const QString &filename,
    const QByteArray &contentChecksum,
    const QByteArray &contentChecksumType)
{
    QMutexLocker locker(&_mutex);

    qCInfo(lcDb) << "Updating file checksum" << filename << contentChecksum << contentChecksumType;

    qlonglong phash = getPHash(filename.toUtf8());
    if (!checkConnect()) {
        qCWarning(lcDb) << "Failed to connect database.";
        return false;
    }

    int checksumTypeId = mapChecksumType(contentChecksumType);

    if (!_setFileRecordChecksumQuery.initOrReset(QByteArrayLiteral(
            "UPDATE metadata"
            " SET contentChecksum = ?2, contentChecksumTypeId = ?3"
            " WHERE phash == ?1;"), _db)) {
        return false;
    }
    _setFileRecordChecksumQuery.bindValue(1, phash);
    _setFileRecordChecksumQuery.bindValue(2, contentChecksum);
    _setFileRecordChecksumQuery.bindValue(3, checksumTypeId);
    return _setFileRecordChecksumQuery.exec();
}

bool SyncJournalDb::updateLocalMetadata(const QString &filename,
    qint64 modtime, quint64 size, quint64 inode)

{
    QMutexLocker locker(&_mutex);

    qCInfo(lcDb) << "Updating local metadata for:" << filename << modtime << size << inode;

    qlonglong phash = getPHash(filename.toUtf8());
    if (!checkConnect()) {
        qCWarning(lcDb) << "Failed to connect database.";
        return false;
    }


    if (!_setFileRecordLocalMetadataQuery.initOrReset(QByteArrayLiteral(
            "UPDATE metadata"
            " SET inode=?2, modtime=?3, filesize=?4"
            " WHERE phash == ?1;"), _db)) {
        return false;
    }

    _setFileRecordLocalMetadataQuery.bindValue(1, phash);
    _setFileRecordLocalMetadataQuery.bindValue(2, inode);
    _setFileRecordLocalMetadataQuery.bindValue(3, modtime);
    _setFileRecordLocalMetadataQuery.bindValue(4, size);
    return _setFileRecordLocalMetadataQuery.exec();
}

bool SyncJournalDb::setFileRecordMetadata(const SyncJournalFileRecord &record)
{
    SyncJournalFileRecord existing;
    if (!getFileRecord(record._path, &existing))
        return false;

    // If there's no existing record, just insert the new one.
    if (!existing.isValid()) {
        return setFileRecord(record);
    }

    // Update the metadata on the existing record.
    existing._inode = record._inode;
    existing._modtime = record._modtime;
    existing._type = record._type;
    existing._etag = record._etag;
    existing._fileId = record._fileId;
    existing._remotePerm = record._remotePerm;
    existing._fileSize = record._fileSize;
    existing._serverHasIgnoredFiles = record._serverHasIgnoredFiles;
    existing._e2eMangledName = record._e2eMangledName;
    existing._isE2eEncrypted = record._isE2eEncrypted;
    return setFileRecord(existing);
}

static void toDownloadInfo(SqlQuery &query, SyncJournalDb::DownloadInfo *res)
{
    bool ok = true;
    res->_tmpfile = query.stringValue(0);
    res->_etag = query.baValue(1);
    res->_errorCount = query.intValue(2);
    res->_valid = ok;
}

static bool deleteBatch(SqlQuery &query, const QStringList &entries, const QString &name)
{
    if (entries.isEmpty())
        return true;

    qCDebug(lcDb) << "Removing stale " << qPrintable(name) << " entries: " << entries.join(", ");
    // FIXME: Was ported from execBatch, check if correct!
    foreach (const QString &entry, entries) {
        query.reset_and_clear_bindings();
        query.bindValue(1, entry);
        if (!query.exec()) {
            return false;
        }
    }

    return true;
}

SyncJournalDb::DownloadInfo SyncJournalDb::getDownloadInfo(const QString &file)
{
    QMutexLocker locker(&_mutex);

    DownloadInfo res;

    if (checkConnect()) {

        if (!_getDownloadInfoQuery.initOrReset(QByteArrayLiteral(
                "SELECT tmpfile, etag, errorcount FROM downloadinfo WHERE path=?1"), _db)) {
            return res;
        }

        _getDownloadInfoQuery.bindValue(1, file);

        if (!_getDownloadInfoQuery.exec()) {
            return res;
        }

        if (_getDownloadInfoQuery.next()) {
            toDownloadInfo(_getDownloadInfoQuery, &res);
        } else {
            res._valid = false;
        }
    }
    return res;
}

void SyncJournalDb::setDownloadInfo(const QString &file, const SyncJournalDb::DownloadInfo &i)
{
    QMutexLocker locker(&_mutex);

    if (!checkConnect()) {
        return;
    }


    if (i._valid) {
        if (!_setDownloadInfoQuery.initOrReset(QByteArrayLiteral(
                "INSERT OR REPLACE INTO downloadinfo "
                "(path, tmpfile, etag, errorcount) "
                "VALUES ( ?1 , ?2, ?3, ?4 )"), _db)) {
            return;
        }
        _setDownloadInfoQuery.bindValue(1, file);
        _setDownloadInfoQuery.bindValue(2, i._tmpfile);
        _setDownloadInfoQuery.bindValue(3, i._etag);
        _setDownloadInfoQuery.bindValue(4, i._errorCount);
        _setDownloadInfoQuery.exec();
    } else {
        _deleteDownloadInfoQuery.reset_and_clear_bindings();
        _deleteDownloadInfoQuery.bindValue(1, file);
        _deleteDownloadInfoQuery.exec();
    }
}

QVector<SyncJournalDb::DownloadInfo> SyncJournalDb::getAndDeleteStaleDownloadInfos(const QSet<QString> &keep)
{
    QVector<SyncJournalDb::DownloadInfo> empty_result;
    QMutexLocker locker(&_mutex);

    if (!checkConnect()) {
        return empty_result;
    }

    SqlQuery query(_db);
    // The selected values *must* match the ones expected by toDownloadInfo().
    query.prepare("SELECT tmpfile, etag, errorcount, path FROM downloadinfo");

    if (!query.exec()) {
        return empty_result;
    }

    QStringList superfluousPaths;
    QVector<SyncJournalDb::DownloadInfo> deleted_entries;

    while (query.next()) {
        const QString file = query.stringValue(3); // path
        if (!keep.contains(file)) {
            superfluousPaths.append(file);
            DownloadInfo info;
            toDownloadInfo(query, &info);
            deleted_entries.append(info);
        }
    }

    if (!deleteBatch(_deleteDownloadInfoQuery, superfluousPaths, "downloadinfo"))
        return empty_result;

    return deleted_entries;
}

int SyncJournalDb::downloadInfoCount()
{
    int re = 0;

    QMutexLocker locker(&_mutex);
    if (checkConnect()) {
        SqlQuery query("SELECT count(*) FROM downloadinfo", _db);

        if (!query.exec()) {
            sqlFail("Count number of downloadinfo entries failed", query);
        }
        if (query.next()) {
            re = query.intValue(0);
        }
    }
    return re;
}

SyncJournalDb::UploadInfo SyncJournalDb::getUploadInfo(const QString &file)
{
    QMutexLocker locker(&_mutex);

    UploadInfo res;

    if (checkConnect()) {
        if (!_getUploadInfoQuery.initOrReset(QByteArrayLiteral(
                "SELECT chunk, transferid, errorcount, size, modtime, contentChecksum FROM "
                "uploadinfo WHERE path=?1"), _db)) {
            return res;
        }
        _getUploadInfoQuery.bindValue(1, file);

        if (!_getUploadInfoQuery.exec()) {
            return res;
        }

        if (_getUploadInfoQuery.next()) {
            bool ok = true;
            res._chunk = _getUploadInfoQuery.intValue(0);
            res._transferid = _getUploadInfoQuery.intValue(1);
            res._errorCount = _getUploadInfoQuery.intValue(2);
            res._size = _getUploadInfoQuery.int64Value(3);
            res._modtime = _getUploadInfoQuery.int64Value(4);
            res._contentChecksum = _getUploadInfoQuery.baValue(5);
            res._valid = ok;
        }
    }
    return res;
}

void SyncJournalDb::setUploadInfo(const QString &file, const SyncJournalDb::UploadInfo &i)
{
    QMutexLocker locker(&_mutex);

    if (!checkConnect()) {
        return;
    }

    if (i._valid) {
        if (!_setUploadInfoQuery.initOrReset(QByteArrayLiteral(
            "INSERT OR REPLACE INTO uploadinfo "
            "(path, chunk, transferid, errorcount, size, modtime, contentChecksum) "
            "VALUES ( ?1 , ?2, ?3 , ?4 ,  ?5, ?6 , ?7 )"), _db)) {
            return;
        }

        _setUploadInfoQuery.bindValue(1, file);
        _setUploadInfoQuery.bindValue(2, i._chunk);
        _setUploadInfoQuery.bindValue(3, i._transferid);
        _setUploadInfoQuery.bindValue(4, i._errorCount);
        _setUploadInfoQuery.bindValue(5, i._size);
        _setUploadInfoQuery.bindValue(6, i._modtime);
        _setUploadInfoQuery.bindValue(7, i._contentChecksum);

        if (!_setUploadInfoQuery.exec()) {
            return;
        }
    } else {
        _deleteUploadInfoQuery.reset_and_clear_bindings();
        _deleteUploadInfoQuery.bindValue(1, file);

        if (!_deleteUploadInfoQuery.exec()) {
            return;
        }
    }
}

QVector<uint> SyncJournalDb::deleteStaleUploadInfos(const QSet<QString> &keep)
{
    QMutexLocker locker(&_mutex);
    QVector<uint> ids;

    if (!checkConnect()) {
        return ids;
    }

    SqlQuery query(_db);
    query.prepare("SELECT path,transferid FROM uploadinfo");

    if (!query.exec()) {
        return ids;
    }

    QStringList superfluousPaths;

    while (query.next()) {
        const QString file = query.stringValue(0);
        if (!keep.contains(file)) {
            superfluousPaths.append(file);
            ids.append(query.intValue(1));
        }
    }

    deleteBatch(_deleteUploadInfoQuery, superfluousPaths, "uploadinfo");
    return ids;
}

SyncJournalErrorBlacklistRecord SyncJournalDb::errorBlacklistEntry(const QString &file)
{
    QMutexLocker locker(&_mutex);
    SyncJournalErrorBlacklistRecord entry;

    if (file.isEmpty())
        return entry;

    // SELECT lastTryEtag, lastTryModtime, retrycount, errorstring

    if (checkConnect()) {
        _getErrorBlacklistQuery.reset_and_clear_bindings();
        _getErrorBlacklistQuery.bindValue(1, file);
        if (_getErrorBlacklistQuery.exec()) {
            if (_getErrorBlacklistQuery.next()) {
                entry._lastTryEtag = _getErrorBlacklistQuery.baValue(0);
                entry._lastTryModtime = _getErrorBlacklistQuery.int64Value(1);
                entry._retryCount = _getErrorBlacklistQuery.intValue(2);
                entry._errorString = _getErrorBlacklistQuery.stringValue(3);
                entry._lastTryTime = _getErrorBlacklistQuery.int64Value(4);
                entry._ignoreDuration = _getErrorBlacklistQuery.int64Value(5);
                entry._renameTarget = _getErrorBlacklistQuery.stringValue(6);
                entry._errorCategory = static_cast<SyncJournalErrorBlacklistRecord::Category>(
                    _getErrorBlacklistQuery.intValue(7));
                entry._file = file;
            }
        }
    }

    return entry;
}

bool SyncJournalDb::deleteStaleErrorBlacklistEntries(const QSet<QString> &keep)
{
    QMutexLocker locker(&_mutex);

    if (!checkConnect()) {
        return false;
    }

    SqlQuery query(_db);
    query.prepare("SELECT path FROM blacklist");

    if (!query.exec()) {
        return false;
    }

    QStringList superfluousPaths;

    while (query.next()) {
        const QString file = query.stringValue(0);
        if (!keep.contains(file)) {
            superfluousPaths.append(file);
        }
    }

    SqlQuery delQuery(_db);
    delQuery.prepare("DELETE FROM blacklist WHERE path = ?");
    return deleteBatch(delQuery, superfluousPaths, "blacklist");
}

int SyncJournalDb::errorBlackListEntryCount()
{
    int re = 0;

    QMutexLocker locker(&_mutex);
    if (checkConnect()) {
        SqlQuery query("SELECT count(*) FROM blacklist", _db);

        if (!query.exec()) {
            sqlFail("Count number of blacklist entries failed", query);
        }
        if (query.next()) {
            re = query.intValue(0);
        }
    }
    return re;
}

int SyncJournalDb::wipeErrorBlacklist()
{
    QMutexLocker locker(&_mutex);
    if (checkConnect()) {
        SqlQuery query(_db);

        query.prepare("DELETE FROM blacklist");

        if (!query.exec()) {
            sqlFail("Deletion of whole blacklist failed", query);
            return -1;
        }
        return query.numRowsAffected();
    }
    return -1;
}

void SyncJournalDb::wipeErrorBlacklistEntry(const QString &file)
{
    if (file.isEmpty()) {
        return;
    }

    QMutexLocker locker(&_mutex);
    if (checkConnect()) {
        SqlQuery query(_db);

        query.prepare("DELETE FROM blacklist WHERE path=?1");
        query.bindValue(1, file);
        if (!query.exec()) {
            sqlFail("Deletion of blacklist item failed.", query);
        }
    }
}

void SyncJournalDb::wipeErrorBlacklistCategory(SyncJournalErrorBlacklistRecord::Category category)
{
    QMutexLocker locker(&_mutex);
    if (checkConnect()) {
        SqlQuery query(_db);

        query.prepare("DELETE FROM blacklist WHERE errorCategory=?1");
        query.bindValue(1, category);
        if (!query.exec()) {
            sqlFail("Deletion of blacklist category failed.", query);
        }
    }
}

void SyncJournalDb::setErrorBlacklistEntry(const SyncJournalErrorBlacklistRecord &item)
{
    QMutexLocker locker(&_mutex);

    qCInfo(lcDb) << "Setting blacklist entry for " << item._file << item._retryCount
                 << item._errorString << item._lastTryTime << item._ignoreDuration
                 << item._lastTryModtime << item._lastTryEtag << item._renameTarget
                 << item._errorCategory;

    if (!checkConnect()) {
        return;
    }

    if (!_setErrorBlacklistQuery.initOrReset(QByteArrayLiteral(
        "INSERT OR REPLACE INTO blacklist "
        "(path, lastTryEtag, lastTryModtime, retrycount, errorstring, lastTryTime, ignoreDuration, renameTarget, errorCategory) "
        "VALUES ( ?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)"), _db)) {
        return;
    }

    _setErrorBlacklistQuery.bindValue(1, item._file);
    _setErrorBlacklistQuery.bindValue(2, item._lastTryEtag);
    _setErrorBlacklistQuery.bindValue(3, item._lastTryModtime);
    _setErrorBlacklistQuery.bindValue(4, item._retryCount);
    _setErrorBlacklistQuery.bindValue(5, item._errorString);
    _setErrorBlacklistQuery.bindValue(6, item._lastTryTime);
    _setErrorBlacklistQuery.bindValue(7, item._ignoreDuration);
    _setErrorBlacklistQuery.bindValue(8, item._renameTarget);
    _setErrorBlacklistQuery.bindValue(9, item._errorCategory);
    _setErrorBlacklistQuery.exec();
}

QVector<SyncJournalDb::PollInfo> SyncJournalDb::getPollInfos()
{
    QMutexLocker locker(&_mutex);

    QVector<SyncJournalDb::PollInfo> res;

    if (!checkConnect())
        return res;

    SqlQuery query("SELECT path, modtime, pollpath FROM poll", _db);

    if (!query.exec()) {
        return res;
    }

    while (query.next()) {
        PollInfo info;
        info._file = query.stringValue(0);
        info._modtime = query.int64Value(1);
        info._url = query.stringValue(2);
        res.append(info);
    }

    query.finish();
    return res;
}

void SyncJournalDb::setPollInfo(const SyncJournalDb::PollInfo &info)
{
    QMutexLocker locker(&_mutex);
    if (!checkConnect()) {
        return;
    }

    if (info._url.isEmpty()) {
        qCDebug(lcDb) << "Deleting Poll job" << info._file;
        SqlQuery query("DELETE FROM poll WHERE path=?", _db);
        query.bindValue(1, info._file);
        query.exec();
    } else {
        SqlQuery query("INSERT OR REPLACE INTO poll (path, modtime, pollpath) VALUES( ? , ? , ? )", _db);
        query.bindValue(1, info._file);
        query.bindValue(2, info._modtime);
        query.bindValue(3, info._url);
        query.exec();
    }
}

QStringList SyncJournalDb::getSelectiveSyncList(SyncJournalDb::SelectiveSyncListType type, bool *ok)
{
    QStringList result;
    ASSERT(ok);

    QMutexLocker locker(&_mutex);
    if (!checkConnect()) {
        *ok = false;
        return result;
    }

    if (!_getSelectiveSyncListQuery.initOrReset(QByteArrayLiteral("SELECT path FROM selectivesync WHERE type=?1"), _db)) {
        *ok = false;
        return result;
    }

    _getSelectiveSyncListQuery.bindValue(1, int(type));
    if (!_getSelectiveSyncListQuery.exec()) {
        *ok = false;
        return result;
    }
    while (_getSelectiveSyncListQuery.next()) {
        auto entry = _getSelectiveSyncListQuery.stringValue(0);
        if (!entry.endsWith(QLatin1Char('/'))) {
            entry.append(QLatin1Char('/'));
        }
        result.append(entry);
    }
    *ok = true;

    return result;
}

void SyncJournalDb::setSelectiveSyncList(SyncJournalDb::SelectiveSyncListType type, const QStringList &list)
{
    QMutexLocker locker(&_mutex);
    if (!checkConnect()) {
        return;
    }

    startTransaction();

    //first, delete all entries of this type
    SqlQuery delQuery("DELETE FROM selectivesync WHERE type == ?1", _db);
    delQuery.bindValue(1, int(type));
    if (!delQuery.exec()) {
        qCWarning(lcDb) << "SQL error when deleting selective sync list" << list << delQuery.error();
    }

    SqlQuery insQuery("INSERT INTO selectivesync VALUES (?1, ?2)", _db);
    foreach (const auto &path, list) {
        insQuery.reset_and_clear_bindings();
        insQuery.bindValue(1, path);
        insQuery.bindValue(2, int(type));
        if (!insQuery.exec()) {
            qCWarning(lcDb) << "SQL error when inserting into selective sync" << type << path << delQuery.error();
        }
    }

    commitInternal("setSelectiveSyncList");
}

void SyncJournalDb::avoidRenamesOnNextSync(const QByteArray &path)
{
    QMutexLocker locker(&_mutex);

    if (!checkConnect()) {
        return;
    }

    SqlQuery query(_db);
    query.prepare("UPDATE metadata SET fileid = '', inode = '0' WHERE " IS_PREFIX_PATH_OR_EQUAL("?1", "path"));
    query.bindValue(1, path);
    query.exec();

    // We also need to remove the ETags so the update phase refreshes the directory paths
    // on the next sync
    avoidReadFromDbOnNextSync(path);
}

void SyncJournalDb::avoidReadFromDbOnNextSync(const QByteArray &fileName)
{
    QMutexLocker locker(&_mutex);

    if (!checkConnect()) {
        return;
    }

    // Remove trailing slash
    auto argument = fileName;
    if (argument.endsWith('/'))
        argument.chop(1);

    SqlQuery query(_db);
    // This query will match entries for which the path is a prefix of fileName
    // Note: CSYNC_FTW_TYPE_DIR == 2
    query.prepare("UPDATE metadata SET md5='_invalid_' WHERE " IS_PREFIX_PATH_OR_EQUAL("path", "?1") " AND type == 2;");
    query.bindValue(1, argument);
    query.exec();

    // Prevent future overwrite of the etags of this folder and all
    // parent folders for this sync
    argument.append('/');
    _etagStorageFilter.append(argument);
}

void SyncJournalDb::clearEtagStorageFilter()
{
    _etagStorageFilter.clear();
}

void SyncJournalDb::forceRemoteDiscoveryNextSync()
{
    QMutexLocker locker(&_mutex);

    if (!checkConnect()) {
        return;
    }

    forceRemoteDiscoveryNextSyncLocked();
}

void SyncJournalDb::forceRemoteDiscoveryNextSyncLocked()
{
    qCInfo(lcDb) << "Forcing remote re-discovery by deleting folder Etags";
    SqlQuery deleteRemoteFolderEtagsQuery(_db);
    deleteRemoteFolderEtagsQuery.prepare("UPDATE metadata SET md5='_invalid_' WHERE type=2;");
    deleteRemoteFolderEtagsQuery.exec();
}


QByteArray SyncJournalDb::getChecksumType(int checksumTypeId)
{
    QMutexLocker locker(&_mutex);
    if (!checkConnect()) {
        return QByteArray();
    }

    // Retrieve the id
    auto &query = _getChecksumTypeQuery;
    if (!query.initOrReset(QByteArrayLiteral("SELECT name FROM checksumtype WHERE id=?1"), _db))
        return {};
    query.bindValue(1, checksumTypeId);
    if (!query.exec()) {
        return nullptr;
    }

    if (!query.next()) {
        qCWarning(lcDb) << "No checksum type mapping found for" << checksumTypeId;
        return nullptr;
    }
    return query.baValue(0);
}

int SyncJournalDb::mapChecksumType(const QByteArray &checksumType)
{
    if (checksumType.isEmpty()) {
        return 0;
    }

    // Ensure the checksum type is in the db
    if (!_insertChecksumTypeQuery.initOrReset(QByteArrayLiteral("INSERT OR IGNORE INTO checksumtype (name) VALUES (?1)"), _db))
        return 0;
    _insertChecksumTypeQuery.bindValue(1, checksumType);
    if (!_insertChecksumTypeQuery.exec()) {
        return 0;
    }

    // Retrieve the id
    if (!_getChecksumTypeIdQuery.initOrReset(QByteArrayLiteral("SELECT id FROM checksumtype WHERE name=?1"), _db))
        return 0;
    _getChecksumTypeIdQuery.bindValue(1, checksumType);
    if (!_getChecksumTypeIdQuery.exec()) {
        return 0;
    }

    if (!_getChecksumTypeIdQuery.next()) {
        qCWarning(lcDb) << "No checksum type mapping found for" << checksumType;
        return 0;
    }
    return _getChecksumTypeIdQuery.intValue(0);
}

QByteArray SyncJournalDb::dataFingerprint()
{
    QMutexLocker locker(&_mutex);
    if (!checkConnect()) {
        return QByteArray();
    }

    if (!_getDataFingerprintQuery.initOrReset(QByteArrayLiteral("SELECT fingerprint FROM datafingerprint"), _db))
        return QByteArray();

    if (!_getDataFingerprintQuery.exec()) {
        return QByteArray();
    }

    if (!_getDataFingerprintQuery.next()) {
        return QByteArray();
    }
    return _getDataFingerprintQuery.baValue(0);
}

void SyncJournalDb::setDataFingerprint(const QByteArray &dataFingerprint)
{
    QMutexLocker locker(&_mutex);
    if (!checkConnect()) {
        return;
    }

    if (!_setDataFingerprintQuery1.initOrReset(QByteArrayLiteral("DELETE FROM datafingerprint;"), _db)
        || !_setDataFingerprintQuery2.initOrReset(QByteArrayLiteral("INSERT INTO datafingerprint (fingerprint) VALUES (?1);"), _db)) {
        return;
    }

    _setDataFingerprintQuery1.exec();

    _setDataFingerprintQuery2.bindValue(1, dataFingerprint);
    _setDataFingerprintQuery2.exec();
}

void SyncJournalDb::setConflictRecord(const ConflictRecord &record)
{
    QMutexLocker locker(&_mutex);
    if (!checkConnect())
        return;

    auto &query = _setConflictRecordQuery;
    ASSERT(query.initOrReset(QByteArrayLiteral(
                          "INSERT OR REPLACE INTO conflicts "
                          "(path, baseFileId, baseModtime, baseEtag) "
                          "VALUES (?1, ?2, ?3, ?4);"),
        _db));
    query.bindValue(1, record.path);
    query.bindValue(2, record.baseFileId);
    query.bindValue(3, record.baseModtime);
    query.bindValue(4, record.baseEtag);
    ASSERT(query.exec());
}

ConflictRecord SyncJournalDb::conflictRecord(const QByteArray &path)
{
    ConflictRecord entry;

    QMutexLocker locker(&_mutex);
    if (!checkConnect())
        return entry;
    auto &query = _getConflictRecordQuery;
    ASSERT(query.initOrReset(QByteArrayLiteral("SELECT baseFileId, baseModtime, baseEtag FROM conflicts WHERE path=?1;"), _db));
    query.bindValue(1, path);
    ASSERT(query.exec());
    if (!query.next())
        return entry;

    entry.path = path;
    entry.baseFileId = query.baValue(0);
    entry.baseModtime = query.int64Value(1);
    entry.baseEtag = query.baValue(2);
    return entry;
}

void SyncJournalDb::deleteConflictRecord(const QByteArray &path)
{
    QMutexLocker locker(&_mutex);
    if (!checkConnect())
        return;

    ASSERT(_deleteConflictRecordQuery.initOrReset("DELETE FROM conflicts WHERE path=?1;", _db));
    _deleteConflictRecordQuery.bindValue(1, path);
    ASSERT(_deleteConflictRecordQuery.exec());
}

QByteArrayList SyncJournalDb::conflictRecordPaths()
{
    QMutexLocker locker(&_mutex);
    if (!checkConnect())
        return {};

    SqlQuery query(_db);
    query.prepare("SELECT path FROM conflicts");
    ASSERT(query.exec());

    QByteArrayList paths;
    while (query.next())
        paths.append(query.baValue(0));

    return paths;
}

void SyncJournalDb::clearFileTable()
{
    QMutexLocker lock(&_mutex);
    SqlQuery query(_db);
    query.prepare("DELETE FROM metadata;");
    query.exec();
}

void SyncJournalDb::commit(const QString &context, bool startTrans)
{
    QMutexLocker lock(&_mutex);
    commitInternal(context, startTrans);
}

void SyncJournalDb::commitIfNeededAndStartNewTransaction(const QString &context)
{
    QMutexLocker lock(&_mutex);
    if (_transaction == 1) {
        commitInternal(context, true);
    } else {
        startTransaction();
    }
}


void SyncJournalDb::commitInternal(const QString &context, bool startTrans)
{
    qCDebug(lcDb) << "Transaction commit " << context << (startTrans ? "and starting new transaction" : "");
    commitTransaction();

    if (startTrans) {
        startTransaction();
    }
}

SyncJournalDb::~SyncJournalDb()
{
    close();
}

bool SyncJournalDb::isConnected()
{
    QMutexLocker lock(&_mutex);
    return checkConnect();
}

bool operator==(const SyncJournalDb::DownloadInfo &lhs,
    const SyncJournalDb::DownloadInfo &rhs)
{
    return lhs._errorCount == rhs._errorCount
        && lhs._etag == rhs._etag
        && lhs._tmpfile == rhs._tmpfile
        && lhs._valid == rhs._valid;
}

bool operator==(const SyncJournalDb::UploadInfo &lhs,
    const SyncJournalDb::UploadInfo &rhs)
{
    return lhs._errorCount == rhs._errorCount
        && lhs._chunk == rhs._chunk
        && lhs._modtime == rhs._modtime
        && lhs._valid == rhs._valid
        && lhs._size == rhs._size
        && lhs._transferid == rhs._transferid
        && lhs._contentChecksum == rhs._contentChecksum;
}

} // namespace OCC
