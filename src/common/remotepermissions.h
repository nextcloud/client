/*
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
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

#pragma once

#include <QString>
#include <QMetaType>
#include "ocsynclib.h"
#include <QDebug>

namespace OCC {

/**
 * Class that store in a memory efficient way the remote permission
 */
class OCSYNC_EXPORT RemotePermissions
{
private:
    // The first bit tells if the value is set or not
    // The remaining bits correspond to know if the value is set
    quint16 _value = 0;
    static constexpr int notNullMask = 0x1;

    template <typename Char> // can be 'char' or 'ushort' if conversion from QString
    void fromArray(const Char *p);

public:
    enum Permissions {
        CanWrite = 1,             // W
        CanDelete = 2,            // D
        CanRename = 3,            // N
        CanMove = 4,              // V
        CanAddFile = 5,           // C
        CanAddSubDirectories = 6, // K
        CanReshare = 7,           // R
        // Note: on the server, this means SharedWithMe, but in discoveryphase.cpp we also set
        // this permission when the server reports the any "share-types"
        IsShared = 8,             // S
        IsMounted = 9,            // M
        IsMountedSub = 10,        // m (internal: set if the parent dir has IsMounted)

        // Note: when adding support for more permissions, we need to invalid the cache in the database.
        // (by setting forceRemoteDiscovery in SyncJournalDb::checkConnect)
        PermissionsCount = IsMountedSub
    };

    /// null permissions
    RemotePermissions() = default;

    /// array with one character per permission, "" is null, " " is non-null but empty
    QByteArray toDbValue() const;

    /// output for display purposes, no defined format (same as toDbValue in practice)
    QString toString() const;

    /// read value that was written with toDbValue()
    static RemotePermissions fromDbValue(const QByteArray &);

    /// read a permissions string received from the server, never null
    static RemotePermissions fromServerString(const QString &);

    bool hasPermission(Permissions p) const
    {
        return _value & (1 << static_cast<int>(p));
    }
    void setPermission(Permissions p)
    {
        _value |= (1 << static_cast<int>(p)) | notNullMask;
    }
    void unsetPermission(Permissions p)
    {
        _value &= ~(1 << static_cast<int>(p));
    }

    bool isNull() const { return !(_value & notNullMask); }
    friend bool operator==(RemotePermissions a, RemotePermissions b)
    {
        return a._value == b._value;
    }
    friend bool operator!=(RemotePermissions a, RemotePermissions b)
    {
        return !(a == b);
    }

    friend QDebug operator<<(QDebug &dbg, RemotePermissions p)
    {
        return dbg << p.toString();
    }
};


} // namespace OCC

Q_DECLARE_METATYPE(OCC::RemotePermissions)
