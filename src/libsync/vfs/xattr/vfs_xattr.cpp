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

#include "vfs_xattr.h"

#include <QFile>

#include "syncfileitem.h"
#include "filesystem.h"
#include "common/syncjournaldb.h"

#include "xattrwrapper.h"

namespace xattr {
using namespace OCC::XAttrWrapper;
}

namespace OCC {

VfsXAttr::VfsXAttr(QObject *parent)
    : Vfs(parent)
{
}

VfsXAttr::~VfsXAttr() = default;

Vfs::Mode VfsXAttr::mode() const
{
    return XAttr;
}

QString VfsXAttr::fileSuffix() const
{
    return QString();
}

void VfsXAttr::startImpl(const VfsSetupParams &)
{
}

void VfsXAttr::stop()
{
}

void VfsXAttr::unregisterFolder()
{
}

bool VfsXAttr::socketApiPinStateActionsShown() const
{
    return true;
}

bool VfsXAttr::isHydrating() const
{
    return false;
}

Result<void, QString> VfsXAttr::updateMetadata(const QString &filePath, time_t modtime, qint64, const QByteArray &)
{
    FileSystem::setModTime(filePath, modtime);
    return {};
}

Result<void, QString> VfsXAttr::createPlaceholder(const SyncFileItem &item)
{
    const auto path = QString(_setupParams.filesystemPath + item._file);
    QFile file(path);
    if (file.exists() && file.size() > 1
        && !FileSystem::verifyFileUnchanged(path, item._size, item._modtime)) {
        return QStringLiteral("Cannot create a placeholder because a file with the placeholder name already exist");
    }

    if (!file.open(QFile::ReadWrite | QFile::Truncate)) {
        return file.errorString();
    }

    file.write(" ");
    file.close();
    FileSystem::setModTime(path, item._modtime);
    return xattr::addNextcloudPlaceholderAttributes(path);
}

Result<void, QString> VfsXAttr::dehydratePlaceholder(const SyncFileItem &item)
{
    const auto path = QString(_setupParams.filesystemPath + item._file);
    QFile file(path);
    if (!file.remove()) {
        return QStringLiteral("Couldn't remove the original file to dehydrate");
    }
    auto r = createPlaceholder(item);
    if (!r) {
        return r;
    }

    // Ensure the pin state isn't contradictory
    const auto pin = pinState(item._file);
    if (pin && *pin == PinState::AlwaysLocal) {
        setPinState(item._renameTarget, PinState::Unspecified);
    }
    return {};
}

Result<Vfs::ConvertToPlaceholderResult, QString> VfsXAttr::convertToPlaceholder(const QString &, const SyncFileItem &, const QString &)
{
    // Nothing necessary
    return {ConvertToPlaceholderResult::Ok};
}

bool VfsXAttr::needsMetadataUpdate(const SyncFileItem &)
{
    return false;
}

bool VfsXAttr::isDehydratedPlaceholder(const QString &filePath)
{
    const auto fi = QFileInfo(filePath);
    return fi.exists() &&
            xattr::hasNextcloudPlaceholderAttributes(filePath);
}

bool VfsXAttr::statTypeVirtualFile(csync_file_stat_t *stat, void *statData)
{
    if (stat->type == ItemTypeDirectory) {
        return false;
    }

    const auto parentPath = static_cast<QByteArray *>(statData);
    Q_ASSERT(!parentPath->endsWith('/'));
    Q_ASSERT(!stat->path.startsWith('/'));

    const auto path = QByteArray(*parentPath + '/' + stat->path);
    const auto pin = [=] {
        const auto absolutePath = QString::fromUtf8(path);
        Q_ASSERT(absolutePath.startsWith(params().filesystemPath.toUtf8()));
        const auto folderPath = absolutePath.mid(params().filesystemPath.length());
        return pinState(folderPath);
    }();

    if (xattr::hasNextcloudPlaceholderAttributes(path)) {
        const auto shouldDownload = pin && (*pin == PinState::AlwaysLocal);
        stat->type = shouldDownload ? ItemTypeVirtualFileDownload : ItemTypeVirtualFile;
        return true;
    } else {
        const auto shouldDehydrate = pin && (*pin == PinState::OnlineOnly);
        if (shouldDehydrate) {
            stat->type = ItemTypeVirtualFileDehydration;
            return true;
        }
    }
    return false;
}

bool VfsXAttr::setPinState(const QString &folderPath, PinState state)
{
    return setPinStateInDb(folderPath, state);
}

Optional<PinState> VfsXAttr::pinState(const QString &folderPath)
{
    return pinStateInDb(folderPath);
}

Vfs::AvailabilityResult VfsXAttr::availability(const QString &folderPath)
{
    return availabilityInDb(folderPath);
}

void VfsXAttr::fileStatusChanged(const QString &, SyncFileStatus)
{
}

} // namespace OCC

OCC_DEFINE_VFS_FACTORY("xattr", OCC::VfsXAttr)
