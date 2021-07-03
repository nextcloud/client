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

#include "foregroundbackground_interface.h"
#include "foregroundbackground_cocoa.h"

bool ForegroundBackground::eventFilter(QObject * /*obj*/, QEvent *event)
{
    if (event->type() == QEvent::Show) {
        ToForeground();
        return true;
    }
    if (event->type() == QEvent::Close) {
        ToBackground();
        return true;
    }
    return false;
}

void ForegroundBackground::ToForeground()
{
    [CocoaProcessType ToForeground];
}

void ForegroundBackground::ToBackground()
{
    [CocoaProcessType ToBackground];
}
