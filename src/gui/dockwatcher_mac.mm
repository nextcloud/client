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

#include "dockwatcher_mac.h"

#import <AppKit/AppKit.h>

#include "folderwatcher.h"


@interface DockIconClickEventHandler : NSObject
- (void)handleDockClickEvent:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)replyEvent;
@end

@implementation DockIconClickEventHandler
- (void)handleDockClickEvent:(NSAppleEventDescriptor*)event withReplyEvent:(NSAppleEventDescriptor*)replyEvent
{
    OCC::Mac::DockWatcher::instance()->emitDockIconClickEvent();
}
@end


namespace OCC {
namespace Mac {

DockIconClickEventHandler* dockIconClickEventHandler = nil;

DockWatcher *DockWatcher::_instance = nullptr;

DockWatcher::DockWatcher(QObject *parent)
    : QObject(parent)
{
    Q_ASSERT(!_instance);
    _instance = this;

    dockIconClickEventHandler = [[DockIconClickEventHandler alloc] init];
}

DockWatcher *DockWatcher::instance(QObject *parent)
{
    if (!_instance) {
        _instance = new DockWatcher(parent);
    }
    return _instance;
}

DockWatcher::~DockWatcher()
{
    if (dockIconClickEventHandler) {
        [dockIconClickEventHandler release];
    }

    _instance = nullptr;
}

void DockWatcher::init()
{
    // com.nextcloud.desktopclient (Info.plist: CFBundleIdentifier)
    _bundleIdentifier = [[NSBundle mainBundle] bundleIdentifier];

    // ~/Library/Preferences/com.apple.dock.plist
    NSString *rootPath = [NSSearchPathForDirectoriesInDomains(NSLibraryDirectory,
       NSUserDomainMask, YES) firstObject];
    _plistDock = [rootPath stringByAppendingPathComponent:@"Preferences/com.apple.dock.plist"];

    // Watcher for plist
    _folderWatcher.reset(new FolderWatcher());
    QObject::connect(_folderWatcher.data(), &FolderWatcher::pathChanged, [this] {
        queryKeepInDock();
    });
    _folderWatcher->init(QString::fromUtf8([_plistDock UTF8String]));

    // Fetch initial state
    queryKeepInDock();

    // Register Click Handler for Dock Icon
    [[NSAppleEventManager sharedAppleEventManager]
     setEventHandler:dockIconClickEventHandler
     andSelector:@selector(handleDockClickEvent:withReplyEvent:)
     forEventClass:kCoreEventClass
     andEventID:kAEReopenApplication];
}

bool DockWatcher::keepInDock() const
{
    return _keepInDock;
}

void DockWatcher::queryKeepInDock()
{
    NSData *plistXML = [[NSFileManager defaultManager] contentsAtPath:_plistDock];

    if (!plistXML) {
        return;
    }

    NSDictionary *dict = (NSDictionary *)[NSPropertyListSerialization
        propertyListWithData:plistXML
        options:NSPropertyListImmutable
        format:nil
        error:nil];

    if (!dict) {
        return;
    }

    /* Example plist's "persistent-apps" element (redacted):
         "tile-data" =         {
             "bundle-identifier" = "com.nextcloud.desktopclient";
         };
     */
    NSArray *apps = [dict objectForKey:@"persistent-apps"];
    bool oldState = _keepInDock;
    _keepInDock = false;

    for (NSArray *key in apps) {
        id tile_data = [key valueForKey:@"tile-data"];

        if (tile_data) {
            id bundle_identifier = [tile_data valueForKey:@"bundle-identifier"];

            if (bundle_identifier &&
                [bundle_identifier compare:_bundleIdentifier] == NSOrderedSame) {
                _keepInDock = true;
                break;
            }
        }
    }

    if (_keepInDock != oldState) {
        emit keepInDockChanged(_keepInDock);
    }
}

void DockWatcher::emitDockIconClickEvent()
{
    emit dockIconClicked();
}

} // namespace Mac
} // namespace OCC
