/*
 * Copyright (C) by Michael Schuster <michael.schuster@nextcloud.com>
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

namespace OCC {

class FolderWatcher;

namespace Mac {

class DockWatcher : public QObject
{
    Q_OBJECT
public:
    ~DockWatcher();
    static DockWatcher *instance(QObject *parent = nullptr);

    void init();

    bool keepInDock() const;

    void emitDockIconClickEvent();

signals:
    void keepInDockChanged(bool keepInDock);
    void dockIconClicked();

private:
    void queryKeepInDock();

    QScopedPointer<FolderWatcher> _folderWatcher;
    bool _keepInDock = false;

    NSString *_bundleIdentifier;
    NSString *_plistDock;

    static DockWatcher *_instance;
    explicit DockWatcher(QObject *parent = nullptr);
};

} // namespace Mac
} // namespace OCC
