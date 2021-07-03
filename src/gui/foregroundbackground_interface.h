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

/**
* @brief ForegroundBackgroundInterface allows ForegroundBackground to be implemented differently per platform
* @ingroup gui
*/

#ifndef FOREGROUNDBACKGROUND_INTERFACE_H
#define FOREGROUNDBACKGROUND_INTERFACE_H

#include <QObject>
#include <QEvent>

namespace OCC {
namespace Ui {

class ForegroundBackground;

}
}

class ForegroundBackground : public QObject
{
    Q_OBJECT

public:

   ForegroundBackground() = default;
   ~ForegroundBackground() = default;

    /**
    * @brief EventFilter catches events that should trigger ForegroundBackground
    * @ingroup gui
    */
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    /**
    * @brief ToForeground() enables the macOS menubar and dock icon, which are necessary for a maximized window to be able to exit full screen.
    * @ingroup gui
    */
    void ToForeground();

    /**
    * @brief ToBackground() disables the macOS menubar and dock icon, so that the application will only be present as a menubar icon.
    * @ingroup gui
    */
    void ToBackground();
};

#endif