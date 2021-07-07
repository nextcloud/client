/*
 * Copyright (C) by Christian Kamm <mail@ckamm.de>
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
#include <QCoreApplication>
#include <QScopedPointer>
#include <QSharedPointer>

#include <memory>

#include "ocsynclib.h"
#include "result.h"
#include "syncfilestatus.h"
#include "pinstate.h"

using csync_file_stat_t = struct csync_file_stat_s;

namespace OCC {

class Account;
using AccountPtr = QSharedPointer<Account>;
class SyncJournalDb;
class VfsPrivate;
class SyncFileItem;

/** Collection of parameters for initializing a Vfs instance. */
struct OCSYNC_EXPORT VfsSetupParams
{
    /** The full path to the folder on the local filesystem
     *
     * Always ends with /.
     */
    QString filesystemPath;

    // Folder display name in Windows Explorer
    QString displayName;

    // Folder alias
    QString alias;

    /** The path to the synced folder on the account
     *
     * Always ends with /.
     */
    QString remotePath;

    /// Account url, credentials etc for network calls
    AccountPtr account;

    /** Access to the sync folder's database.
     *
     * Note: The journal must live at least until the Vfs::stop() call.
     */
    SyncJournalDb *journal = nullptr;

    /// Strings potentially passed on to the platform
    QString providerName;
    QString providerVersion;

    /** when registering with the system we might use
     *  a different presentaton to identify the accounts
     */
    bool multipleAccountsRegistered = false;
};

/** Interface describing how to deal with virtual/placeholder files.
 *
 * There are different ways of representing files locally that will only
 * be filled with data (hydrated) on demand. One such way would be suffixed
 * files, others could be FUSE based or use Windows CfAPI.
 *
 * This interface intends to decouple the sync algorithm and Folder from
 * the details of how a particular VFS solution works.
 *
 * An instance is usually created through a plugin via the createVfsFromPlugin()
 * function.
 */
class OCSYNC_EXPORT Vfs : public QObject
{
    Q_OBJECT

public:
    /** The kind of VFS in use (or no-VFS)
     *
     * Currently plugins and modes are one-to-one but that's not required.
     */
    enum Mode
    {
        Off,
        WithSuffix,
        WindowsCfApi,
        XAttr,
    };
    Q_ENUM(Mode)
    enum class ConvertToPlaceholderResult {
        Error,
        Ok,
        Locked
    };
    Q_ENUM(ConvertToPlaceholderResult)

    static QString modeToString(Mode mode);
    static Optional<Mode> modeFromString(const QString &str);

    static Result<bool, QString> checkAvailability(const QString &path);

    enum class AvailabilityError
    {
        // Availability can't be retrieved due to db error
        DbError,
        // Availability not available since the item doesn't exist
        NoSuchItem,
    };
    using AvailabilityResult = Result<VfsItemAvailability, AvailabilityError>;

public:
    using Factory = Vfs* (*)();
    static void registerPlugin(const QString &name, Factory factory);

    explicit Vfs(QObject* parent = nullptr);
    virtual ~Vfs();

    virtual Mode mode() const = 0;

    /// For WithSuffix modes: the suffix (including the dot)
    virtual QString fileSuffix() const = 0;

    /// Access to the parameters the instance was start()ed with.
    const VfsSetupParams &params() const { return _setupParams; }


    /** Initializes interaction with the VFS provider.
     *
     * The plugin-specific work is done in startImpl().
     */
    void start(const VfsSetupParams &params);

    /// Stop interaction with VFS provider. Like when the client application quits.
    virtual void stop() = 0;

    /// Deregister the folder with the sync provider, like when a folder is removed.
    virtual void unregisterFolder() = 0;


    /** Whether the socket api should show pin state options
     *
     * Some plugins might provide alternate shell integration, making the normal
     * context menu actions redundant.
     */
    virtual bool socketApiPinStateActionsShown() const = 0;

    /** Return true when download of a file's data is currently ongoing.
     *
     * See also the beginHydrating() and doneHydrating() signals.
     */
    virtual bool isHydrating() const = 0;

    /** Update placeholder metadata during discovery.
     *
     * If the remote metadata changes, the local placeholder's metadata should possibly
     * change as well.
     */
    virtual Q_REQUIRED_RESULT Result<void, QString> updateMetadata(const QString &filePath, time_t modtime, qint64 size, const QByteArray &fileId) = 0;

    /// Create a new dehydrated placeholder. Called from PropagateDownload.
    virtual Q_REQUIRED_RESULT Result<void, QString> createPlaceholder(const SyncFileItem &item) = 0;

    /** Convert a hydrated placeholder to a dehydrated one. Called from PropagateDownlaod.
     *
     * This is different from delete+create because preserving some file metadata
     * (like pin states) may be essential for some vfs plugins.
     */
    virtual Q_REQUIRED_RESULT Result<void, QString> dehydratePlaceholder(const SyncFileItem &item) = 0;

    /** Discovery hook: even unchanged files may need UPDATE_METADATA.
     *
     * For instance cfapi vfs wants local hydrated non-placeholder files to
     * become hydrated placeholder files.
     */
    virtual Q_REQUIRED_RESULT bool needsMetadataUpdate(const SyncFileItem &item) = 0;

    /** Convert a new file to a hydrated placeholder.
     *
     * Some VFS integrations expect that every file, including those that have all
     * the remote data, are "placeholders". This function is called by PropagateDownload
     * to convert newly downloaded, fully hydrated files into placeholders.
     *
     * Implementations must make sure that calling this function on a file that already
     * is a placeholder is acceptable.
     *
     * replacesFile can optionally contain a filesystem path to a placeholder that this
     * new placeholder shall supersede, for rename-replace actions with new downloads,
     * for example.
     */
    virtual Q_REQUIRED_RESULT Result<Vfs::ConvertToPlaceholderResult, QString> convertToPlaceholder(
        const QString &filename,
        const SyncFileItem &item,
        const QString &replacesFile = QString()) = 0;

    /// Determine whether the file at the given absolute path is a dehydrated placeholder.
    virtual Q_REQUIRED_RESULT bool isDehydratedPlaceholder(const QString &filePath) = 0;

    /** Similar to isDehydratedPlaceholder() but used from sync discovery.
     *
     * This function shall set stat->type if appropriate.
     * It may rely on stat->path and stat_data (platform specific data).
     *
     * Returning true means that type was fully determined.
     */
    virtual Q_REQUIRED_RESULT bool statTypeVirtualFile(csync_file_stat_t *stat, void *stat_data) = 0;

    /** Sets the pin state for the item at a path.
     *
     * The pin state is set on the item and for all items below it.
     *
     * Usually this would forward to setting the pin state flag in the db table,
     * but some vfs plugins will store the pin state in file attributes instead.
     *
     * folderPath is relative to the sync folder. Can be "" for root folder.
     */
    virtual Q_REQUIRED_RESULT bool setPinState(const QString &folderPath, PinState state) = 0;

    /** Returns the pin state of an item at a path.
     *
     * Usually backed by the db's effectivePinState() function but some vfs
     * plugins will override it to retrieve the state from elsewhere.
     *
     * folderPath is relative to the sync folder. Can be "" for root folder.
     *
     * Returns none on retrieval error.
     */
    virtual Q_REQUIRED_RESULT Optional<PinState> pinState(const QString &folderPath) = 0;

    /** Returns availability status of an item at a path.
     *
     * The availability is a condensed user-facing version of PinState. See
     * VfsItemAvailability for details.
     *
     * folderPath is relative to the sync folder. Can be "" for root folder.
     */
    virtual Q_REQUIRED_RESULT AvailabilityResult availability(const QString &folderPath) = 0;

public slots:
    /** Update in-sync state based on SyncFileStatusTracker signal.
     *
     * For some vfs plugins the icons aren't based on SocketAPI but rather on data shared
     * via the vfs plugin. The connection to SyncFileStatusTracker allows both to be based
     * on the same data.
     */
    virtual void fileStatusChanged(const QString &systemFileName, SyncFileStatus fileStatus) = 0;

signals:
    /// Emitted when a user-initiated hydration starts
    void beginHydrating();
    /// Emitted when the hydration ends
    void doneHydrating();

protected:
    /** Setup the plugin for the folder.
     *
     * For example, the VFS provider might monitor files to be able to start a file
     * hydration (download of a file's remote contents) when the user wants to open
     * it.
     *
     * Usually some registration needs to be done with the backend. This function
     * should take care of it if necessary.
     */
    virtual void startImpl(const VfsSetupParams &params) = 0;

    // Db-backed pin state handling. Derived classes may use it to implement pin states.
    bool setPinStateInDb(const QString &folderPath, PinState state);
    Optional<PinState> pinStateInDb(const QString &folderPath);
    AvailabilityResult availabilityInDb(const QString &folderPath);

    // the parameters passed to start()
    VfsSetupParams _setupParams;
};

/// Implementation of Vfs for Vfs::Off mode - does nothing
class OCSYNC_EXPORT VfsOff : public Vfs
{
    Q_OBJECT

public:
    VfsOff(QObject* parent = nullptr);
    virtual ~VfsOff();

    Mode mode() const override { return Vfs::Off; }

    QString fileSuffix() const override { return QString(); }

    void stop() override {}
    void unregisterFolder() override {}

    bool socketApiPinStateActionsShown() const override { return false; }
    bool isHydrating() const override { return false; }

    Result<void, QString> updateMetadata(const QString &, time_t, qint64, const QByteArray &) override { return {}; }
    Result<void, QString> createPlaceholder(const SyncFileItem &) override { return {}; }
    Result<void, QString> dehydratePlaceholder(const SyncFileItem &) override { return {}; }
    Result<ConvertToPlaceholderResult, QString> convertToPlaceholder(const QString &, const SyncFileItem &, const QString &) override { return ConvertToPlaceholderResult::Ok; }

    bool needsMetadataUpdate(const SyncFileItem &) override { return false; }
    bool isDehydratedPlaceholder(const QString &) override { return false; }
    bool statTypeVirtualFile(csync_file_stat_t *, void *) override { return false; }

    bool setPinState(const QString &, PinState) override { return true; }
    Optional<PinState> pinState(const QString &) override { return PinState::AlwaysLocal; }
    AvailabilityResult availability(const QString &) override { return VfsItemAvailability::AlwaysLocal; }

public slots:
    void fileStatusChanged(const QString &, SyncFileStatus) override {}

protected:
    void startImpl(const VfsSetupParams &) override {}
};

/// Check whether the plugin for the mode is available.
OCSYNC_EXPORT bool isVfsPluginAvailable(Vfs::Mode mode);

/// Return the best available VFS mode.
OCSYNC_EXPORT Vfs::Mode bestAvailableVfsMode();

/// Create a VFS instance for the mode, returns nullptr on failure.
OCSYNC_EXPORT std::unique_ptr<Vfs> createVfsFromPlugin(Vfs::Mode mode);

} // namespace OCC

#define OCC_DEFINE_VFS_FACTORY(name, Type) \
    static_assert (std::is_base_of<OCC::Vfs, Type>::value, "Please define VFS factories only for OCC::Vfs subclasses"); \
    namespace { \
    void initPlugin() \
    { \
        OCC::Vfs::registerPlugin(QStringLiteral(name), []() -> OCC::Vfs * { return new (Type); }); \
    } \
    Q_COREAPP_STARTUP_FUNCTION(initPlugin) \
    }
