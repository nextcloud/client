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
#pragma once

#include <memory>

#include "cfapiexport.h"
#include "common/pinstate.h"
#include "common/result.h"
#include "common/vfs.h"

struct CF_PLACEHOLDER_BASIC_INFO;

namespace OCC {

class VfsCfApi;

namespace CfApiWrapper
{

class NEXTCLOUD_CFAPI_EXPORT ConnectionKey
{
public:
    ConnectionKey();
    inline void *get() const { return _data.get(); }

private:
    std::unique_ptr<void, void(*)(void *)> _data;
};

class NEXTCLOUD_CFAPI_EXPORT FileHandle
{
public:
    using Deleter = void (*)(void *);

    FileHandle();
    FileHandle(void *data, Deleter deleter);

    inline void *get() const { return _data.get(); }
    inline explicit operator bool() const noexcept { return static_cast<bool>(_data); }

private:
    std::unique_ptr<void, void(*)(void *)> _data;
};

class NEXTCLOUD_CFAPI_EXPORT PlaceHolderInfo
{
public:
    using Deleter = void (*)(CF_PLACEHOLDER_BASIC_INFO *);

    PlaceHolderInfo();
    PlaceHolderInfo(CF_PLACEHOLDER_BASIC_INFO *data, Deleter deleter);

    inline CF_PLACEHOLDER_BASIC_INFO *get() const noexcept { return _data.get(); }
    inline CF_PLACEHOLDER_BASIC_INFO *operator->() const noexcept { return _data.get(); }
    inline explicit operator bool() const noexcept { return static_cast<bool>(_data); }

    Optional<PinState> pinState() const;

private:
    std::unique_ptr<CF_PLACEHOLDER_BASIC_INFO, Deleter> _data;
};

NEXTCLOUD_CFAPI_EXPORT Result<void, QString> registerSyncRoot(const QString &path, const QString &providerName, const QString &providerVersion, const QString &folderAlias, const QString &displayName, const QString &accountDisplayName);
NEXTCLOUD_CFAPI_EXPORT Result<void, QString> unregisterSyncRoot(const QString &path, const QString &providerName, const QString &accountDisplayName);

NEXTCLOUD_CFAPI_EXPORT Result<ConnectionKey, QString> connectSyncRoot(const QString &path, VfsCfApi *context);
NEXTCLOUD_CFAPI_EXPORT Result<void, QString> disconnectSyncRoot(ConnectionKey &&key);

NEXTCLOUD_CFAPI_EXPORT bool isSparseFile(const QString &path);

NEXTCLOUD_CFAPI_EXPORT FileHandle handleForPath(const QString &path);

PlaceHolderInfo findPlaceholderInfo(const QString & path);

enum SetPinRecurseMode {
    NoRecurse = 0,
    Recurse,
    ChildrenOnly
};

NEXTCLOUD_CFAPI_EXPORT Result<OCC::Vfs::ConvertToPlaceholderResult, QString> setPinState(const QString & path, PinState state, SetPinRecurseMode mode);
NEXTCLOUD_CFAPI_EXPORT Result<void, QString> createPlaceholderInfo(const QString &path, time_t modtime, qint64 size, const QByteArray &fileId);
NEXTCLOUD_CFAPI_EXPORT Result<OCC::Vfs::ConvertToPlaceholderResult, QString> updatePlaceholderInfo(const QString &path, time_t modtime, qint64 size, const QByteArray &fileId, const QString &replacesPath = QString());
NEXTCLOUD_CFAPI_EXPORT Result<OCC::Vfs::ConvertToPlaceholderResult, QString> convertToPlaceholder(const QString &path, time_t modtime, qint64 size, const QByteArray &fileId, const QString &replacesPath);

}

} // namespace OCC
