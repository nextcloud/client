/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 * Copyright (C) by Krzesimir Nowak <krzesimir@endocode.com>
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

#ifndef MIRALL_OWNCLOUD_WIZARD_COMMON_H
#define MIRALL_OWNCLOUD_WIZARD_COMMON_H

#include <QString>

class QVariant;
class QLabel;
class QRadioButton;
class QSpinBox;
class QCheckBox;
class QAbstractButton;

namespace OCC {

namespace WizardCommon {

    void setupCustomMedia(const QVariant &variant, QLabel *label);
    QString titleTemplate();
    QString subTitleTemplate();
    void initErrorLabel(QLabel *errorLabel);
    void customizeHintLabel(QLabel *label);

    enum SyncMode {
        SelectiveMode,
        BoxMode
    };

    enum Pages {
        Page_Welcome,
        Page_ServerSetup,
        Page_HttpCreds,
        Page_OAuthCreds,
        Page_Flow2AuthCreds,
#ifdef WITH_WEBENGINE
        Page_WebView,
#endif // WITH_WEBENGINE
        Page_AdvancedSetup,
        Page_Result
    };

} // ns WizardCommon

} // namespace OCC

#endif // MIRALL_OWNCLOUD_WIZARD_COMMON_H
