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

#include "foregroundbackground_cocoa.h"
#include "common/utility.h"

@implementation CocoaProcessType

+ (void)ToForeground
{
        NSApplicationLoad();
        ProcessSerialNumber processSerialNumber = { 0, kCurrentProcess };
        TransformProcessType(&processSerialNumber, kProcessTransformToForegroundApplication);
}

+ (void)ToBackground
{
        NSApplicationLoad();
        ProcessSerialNumber processSerialNumber = { 0, kCurrentProcess };
        TransformProcessType(&processSerialNumber, kProcessTransformToUIElementApplication);
}

@end