/*
 * Copyright (C) by Kevin Ottens <kevin.ottens@nextcloud.com>
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

#include "vfs_cfapi.h"

#include <QDir>
#include <QFile>
#include <QMessageBox>

#include "cfapiwrapper.h"
#include "hydrationjob.h"
#include "syncfileitem.h"
#include "filesystem.h"
#include "common/syncjournaldb.h"

#include <cfapi.h>
#include <comdef.h>

Q_LOGGING_CATEGORY(lcCfApi, "nextcloud.sync.vfs.cfapi", QtInfoMsg)

namespace cfapi {
using namespace OCC::CfApiWrapper;
}

namespace OCC {

class VfsCfApiPrivate
{
public:
    QList<HydrationJob *> hydrationJobs;
    cfapi::ConnectionKey connectionKey;
};

VfsCfApi::VfsCfApi(QObject *parent)
    : Vfs(parent)
    , d(new VfsCfApiPrivate)
{
}

VfsCfApi::~VfsCfApi() = default;

Vfs::Mode VfsCfApi::mode() const
{
    return WindowsCfApi;
}

QString VfsCfApi::fileSuffix() const
{
    return {};
}

void VfsCfApi::startImpl(const VfsSetupParams &params)
{
    const auto localPath = QDir::toNativeSeparators(params.filesystemPath);

    const auto registerResult = cfapi::registerSyncRoot(localPath, params.providerName, params.providerVersion, params.alias, params.displayName, params.account->displayName());
    if (!registerResult) {
        qCCritical(lcCfApi) << "Initialization failed, couldn't register sync root:" << registerResult.error();
        return;
    }

    auto connectResult = cfapi::connectSyncRoot(localPath, this);
    if (!connectResult) {
        qCCritical(lcCfApi) << "Initialization failed, couldn't connect sync root:" << connectResult.error();
        return;
    }

    d->connectionKey = *std::move(connectResult);
}

void VfsCfApi::stop()
{
    const auto result = cfapi::disconnectSyncRoot(std::move(d->connectionKey));
    if (!result) {
        qCCritical(lcCfApi) << "Disconnect failed for" << QDir::toNativeSeparators(params().filesystemPath) << ":" << result.error();
    }
}

void VfsCfApi::unregisterFolder()
{
    const auto localPath = QDir::toNativeSeparators(params().filesystemPath);
    const auto result = cfapi::unregisterSyncRoot(localPath, params().providerName, params().account->displayName());
    if (!result) {
        qCCritical(lcCfApi) << "Unregistration failed for" << localPath << ":" << result.error();
    }
}

bool VfsCfApi::socketApiPinStateActionsShown() const
{
    return true;
}

bool VfsCfApi::isHydrating() const
{
    return !d->hydrationJobs.isEmpty();
}

Result<void, QString> VfsCfApi::updateMetadata(const QString &filePath, time_t modtime, qint64 size, const QByteArray &fileId)
{
    const auto localPath = QDir::toNativeSeparators(filePath);
    const auto handle = cfapi::handleForPath(localPath);
    if (handle) {
        auto result = cfapi::updatePlaceholderInfo(handle, modtime, size, fileId);
        if (result) {
            return {};
        } else {
            return result.error();
        }
    } else {
        qCWarning(lcCfApi) << "Couldn't update metadata for non existing file" << localPath;
        return QStringLiteral("Couldn't update metadata");
    }
}

Result<void, QString> VfsCfApi::createPlaceholder(const SyncFileItem &item)
{
    Q_ASSERT(params().filesystemPath.endsWith('/'));
    const auto localPath = QDir::toNativeSeparators(params().filesystemPath + item._file);
    const auto result = cfapi::createPlaceholderInfo(localPath, item._modtime, item._size, item._fileId);
    return result;
}

Result<void, QString> VfsCfApi::dehydratePlaceholder(const SyncFileItem &item)
{
    const auto previousPin = pinState(item._file);

    if (!FileSystem::remove(_setupParams.filesystemPath + item._file)) {
        return QStringLiteral("Couldn't remove %1 to fulfill dehydration").arg(item._file);
    }

    const auto r = createPlaceholder(item);
    if (!r) {
        return r;
    }

    if (previousPin) {
        if (*previousPin == PinState::AlwaysLocal) {
            setPinState(item._file, PinState::Unspecified);
        } else {
            setPinState(item._file, *previousPin);
        }
    }

    return {};
}

Result<Vfs::ConvertToPlaceholderResult, QString> VfsCfApi::convertToPlaceholder(const QString &filename, const SyncFileItem &item, const QString &replacesFile)
{
    const auto localPath = QDir::toNativeSeparators(filename);
    const auto replacesPath = QDir::toNativeSeparators(replacesFile);

    const auto handle = cfapi::handleForPath(localPath);
    if (cfapi::findPlaceholderInfo(handle)) {
        return cfapi::updatePlaceholderInfo(handle, item._modtime, item._size, item._fileId, replacesPath);
    } else {
        return cfapi::convertToPlaceholder(handle, item._modtime, item._size, item._fileId, replacesPath);
    }
}

bool VfsCfApi::needsMetadataUpdate(const SyncFileItem &item)
{
    return false;
}

bool VfsCfApi::isDehydratedPlaceholder(const QString &filePath)
{
    const auto path = QDir::toNativeSeparators(filePath);
    return cfapi::isSparseFile(path);
}

bool VfsCfApi::statTypeVirtualFile(csync_file_stat_t *stat, void *statData)
{
    const auto ffd = static_cast<WIN32_FIND_DATA *>(statData);

    const auto isDirectory = (ffd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const auto isSparseFile = (ffd->dwFileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0;
    const auto isPinned = (ffd->dwFileAttributes & FILE_ATTRIBUTE_PINNED) != 0;
    const auto isUnpinned = (ffd->dwFileAttributes & FILE_ATTRIBUTE_UNPINNED) != 0;
    const auto hasReparsePoint = (ffd->dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    const auto hasCloudTag = (ffd->dwReserved0 & IO_REPARSE_TAG_CLOUD) != 0;

    const auto isWindowsShortcut = !isDirectory && FileSystem::isLnkFile(stat->path);

    const auto isExcludeFile = !isDirectory && FileSystem::isExcludeFile(stat->path);

    // It's a dir with a reparse point due to the placeholder info (hence the cloud tag)
    // if we don't remove the reparse point flag the discovery will end up thinking
    // it is a file... let's prevent it
    if (isDirectory) {
        if (hasReparsePoint && hasCloudTag) {
            ffd->dwFileAttributes &= ~FILE_ATTRIBUTE_REPARSE_POINT;
        }
        return false;
    } else if (isSparseFile && isPinned) {
        stat->type = ItemTypeVirtualFileDownload;
        return true;
    } else if (!isSparseFile && isUnpinned && !isWindowsShortcut && !isExcludeFile) {
        stat->type = ItemTypeVirtualFileDehydration;
        return true;
    } else if (isSparseFile) {
        stat->type = ItemTypeVirtualFile;
        return true;
    }

    return false;
}

bool VfsCfApi::setPinState(const QString &folderPath, PinState state)
{
    const auto localPath = QDir::toNativeSeparators(params().filesystemPath + folderPath);
    const auto handle = cfapi::handleForPath(localPath);
    if (handle) {
        if (cfapi::setPinState(handle, state, cfapi::Recurse)) {
            return true;
        } else {
            return false;
        }
    } else {
        qCWarning(lcCfApi) << "Couldn't update pin state for non existing file" << localPath;
        return false;
    }
}

Optional<PinState> VfsCfApi::pinState(const QString &folderPath)
{
    const auto localPath = QDir::toNativeSeparators(params().filesystemPath + folderPath);
    const auto handle = cfapi::handleForPath(localPath);
    if (!handle) {
        qCWarning(lcCfApi) << "Couldn't find pin state for non existing file" << localPath;
        return {};
    }

    const auto info = cfapi::findPlaceholderInfo(handle);
    if (!info) {
        qCWarning(lcCfApi) << "Couldn't find pin state for regular non-placeholder file" << localPath;
        return {};
    }

    return info.pinState();
}

Vfs::AvailabilityResult VfsCfApi::availability(const QString &folderPath)
{
    const auto basePinState = pinState(folderPath);
    const auto hydrationAndPinStates = computeRecursiveHydrationAndPinStates(folderPath, basePinState);

    const auto pin = hydrationAndPinStates.pinState;
    const auto hydrationStatus = hydrationAndPinStates.hydrationStatus;

    if (hydrationStatus.hasDehydrated) {
        if (hydrationStatus.hasHydrated)
            return VfsItemAvailability::Mixed;
        if (pin && *pin == PinState::OnlineOnly)
            return VfsItemAvailability::OnlineOnly;
        else
            return VfsItemAvailability::AllDehydrated;
    } else if (hydrationStatus.hasHydrated) {
        if (pin && *pin == PinState::AlwaysLocal)
            return VfsItemAvailability::AlwaysLocal;
        else
            return VfsItemAvailability::AllHydrated;
    }
    return AvailabilityError::NoSuchItem;
}

HydrationJob *VfsCfApi::findHydrationJob(const QString &requestId) const
{
    // Find matching hydration job for request id
    const auto hydrationJobsIter = std::find_if(d->hydrationJobs.cbegin(), d->hydrationJobs.cend(), [&](const HydrationJob *job) {
        return job->requestId() == requestId;
    });

    if (hydrationJobsIter != d->hydrationJobs.cend()) {
        return *hydrationJobsIter;
    }

    return nullptr;
}

void VfsCfApi::cancelHydration(const QString &requestId, const QString & /*path*/)
{
    // Find matching hydration job for request id
    const auto hydrationJob = findHydrationJob(requestId);
    // If found, cancel it
    if (hydrationJob) {
        qCInfo(lcCfApi) << "Cancel hydration";
        hydrationJob->cancel();
    }
}

void VfsCfApi::requestHydration(const QString &requestId, const QString &path)
{
    qCInfo(lcCfApi) << "Received request to hydrate" << path << requestId;
    const auto root = QDir::toNativeSeparators(params().filesystemPath);
    Q_ASSERT(path.startsWith(root));

    const auto relativePath = QDir::fromNativeSeparators(path.mid(root.length()));
    const auto journal = params().journal;

    // Set in the database that we should download the file
    SyncJournalFileRecord record;
    journal->getFileRecord(relativePath, &record);
    if (!record.isValid()) {
        qCInfo(lcCfApi) << "Couldn't hydrate, did not find file in db";
        emit hydrationRequestFailed(requestId);
        return;
    }

    if (!record.isVirtualFile()) {
        qCInfo(lcCfApi) << "Couldn't hydrate, the file is not virtual";
        emit hydrationRequestFailed(requestId);
        return;
    }

    // This is impossible to handle with CfAPI since the file size is generally different
    // between the encrypted and the decrypted file which would make CfAPI reject the hydration
    // of the placeholder with decrypted data
    if (record._isE2eEncrypted || !record._e2eMangledName.isEmpty()) {
        qCInfo(lcCfApi) << "Couldn't hydrate, the file is E2EE this is not supported";

        QMessageBox e2eeFileDownloadRequestWarningMsgBox;
        e2eeFileDownloadRequestWarningMsgBox.setText(tr("Download of end-to-end encrypted file failed"));
        e2eeFileDownloadRequestWarningMsgBox.setInformativeText(tr("It seems that you are trying to download a virtual file that"
                                                                   " is end-to-end encrypted. Implicitly downloading such files is not"
                                                                   " supported at the moment. To workaround this issue, go to the"
                                                                   " settings and mark the encrypted folder with \"Make always available"
                                                                   " locally\"."));
        e2eeFileDownloadRequestWarningMsgBox.setIcon(QMessageBox::Warning);
        e2eeFileDownloadRequestWarningMsgBox.exec();

        emit hydrationRequestFailed(requestId);
        return;
    }

    // All good, let's hydrate now
    scheduleHydrationJob(requestId, relativePath);
}

void VfsCfApi::fileStatusChanged(const QString &systemFileName, SyncFileStatus fileStatus)
{
    Q_UNUSED(systemFileName);
    Q_UNUSED(fileStatus);
}

void VfsCfApi::scheduleHydrationJob(const QString &requestId, const QString &folderPath)
{
    const auto jobAlreadyScheduled = std::any_of(std::cbegin(d->hydrationJobs), std::cend(d->hydrationJobs), [=](HydrationJob *job) {
        return job->requestId() == requestId || job->folderPath() == folderPath;
    });

    if (jobAlreadyScheduled) {
        qCWarning(lcCfApi) << "The OS submitted again a hydration request which is already on-going" << requestId << folderPath;
        emit hydrationRequestFailed(requestId);
        return;
    }

    if (d->hydrationJobs.isEmpty()) {
        emit beginHydrating();
    }

    auto job = new HydrationJob(this);
    job->setAccount(params().account);
    job->setRemotePath(params().remotePath);
    job->setLocalPath(params().filesystemPath);
    job->setJournal(params().journal);
    job->setRequestId(requestId);
    job->setFolderPath(folderPath);
    connect(job, &HydrationJob::finished, this, &VfsCfApi::onHydrationJobFinished);
    d->hydrationJobs << job;
    job->start();
    emit hydrationRequestReady(requestId);
}

void VfsCfApi::onHydrationJobFinished(HydrationJob *job)
{
    Q_ASSERT(d->hydrationJobs.contains(job));
    qCInfo(lcCfApi) << "Hydration job finished" << job->requestId() << job->folderPath() << job->status();
    emit hydrationRequestFinished(job->requestId());
}

int VfsCfApi::finalizeHydrationJob(const QString &requestId)
{
    qCDebug(lcCfApi) << "Finalize hydration job" << requestId;
    // Find matching hydration job for request id
    const auto hydrationJob = findHydrationJob(requestId);

    // If found, finalize it
    if (hydrationJob) {
        hydrationJob->finalize(this);
        d->hydrationJobs.removeAll(hydrationJob);
        hydrationJob->deleteLater();
        if (d->hydrationJobs.isEmpty()) {
            emit doneHydrating();
        }
        return hydrationJob->status();
    }

    return HydrationJob::Status::Error;
}

VfsCfApi::HydratationAndPinStates VfsCfApi::computeRecursiveHydrationAndPinStates(const QString &folderPath, const Optional<PinState> &basePinState)
{
    Q_ASSERT(!folderPath.endsWith('/'));
    QFileInfo info(params().filesystemPath + folderPath);

    if (!info.exists()) {
        return {};
    }
    const auto effectivePin = pinState(folderPath);
    const auto pinResult = (!effectivePin && !basePinState) ? Optional<PinState>()
                         : (!effectivePin || !basePinState) ? PinState::Inherited
                         : (*effectivePin == *basePinState) ? *effectivePin
                         : PinState::Inherited;

    if (info.isDir()) {
        const auto dirState = HydratationAndPinStates {
            pinResult,
            {}
        };
        const auto dir = QDir(info.absoluteFilePath());
        Q_ASSERT(dir.exists());
        const auto children = dir.entryList();
        return std::accumulate(std::cbegin(children), std::cend(children), dirState, [=](const HydratationAndPinStates &currentState, const QString &name) {
            if (name == QStringLiteral("..") || name == QStringLiteral(".")) {
                return currentState;
            }

            // if the folderPath.isEmpty() we don't want to end up having path "/example.file" because this will lead to double slash later, when appending to "SyncFolder/"
            const auto path = folderPath.isEmpty() ? name : folderPath + '/' + name;
            const auto states = computeRecursiveHydrationAndPinStates(path, currentState.pinState);
            return HydratationAndPinStates {
                states.pinState,
                {
                    states.hydrationStatus.hasHydrated || currentState.hydrationStatus.hasHydrated,
                    states.hydrationStatus.hasDehydrated || currentState.hydrationStatus.hasDehydrated,
                }
            };
        });
    } else { // file case
        const auto isDehydrated = isDehydratedPlaceholder(info.absoluteFilePath());
        return {
            pinResult,
            {
                !isDehydrated,
                isDehydrated
            }
        };
    }
}

} // namespace OCC

OCC_DEFINE_VFS_FACTORY("win", OCC::VfsCfApi)
