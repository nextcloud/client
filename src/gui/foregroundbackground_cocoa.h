/*
 * Copyright (C) by Elsie Hupp <gpl at elsiehupp dot com>
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

#import <AppKit/NSApplication.h>

/**
* @brief CocoaProcessType provides methods for moving the application between
* the background and foreground.
* @ingroup gui
*/

#ifndef COCOAPROCESSTYPE_H
#define COCOAPROCESSTYPE_H

@interface CocoaProcessType : NSApplication

/**
 * @brief CocoaProcessTypeToForeground() enables the macOS menubar and dock icon, which are necessary for a maximized window to be able to exit full screen.
 * @ingroup gui
 */
+ (void)ToForeground;

/**
 * @brief CocoaProcessTypeToBackground() disables the macOS menubar and dock icon, so that the application will only be present as a menubar icon.
 * @ingroup gui
 */
+ (void)ToBackground;

@end

#endif