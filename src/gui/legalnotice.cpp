/*
 * Copyright (C) by Roeland Jago Douma <roeland@famdouma.nl>
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

#include "legalnotice.h"
#include "ui_legalnotice.h"
#include "theme.h"
#include "owncloudgui.h"

namespace OCC {


LegalNotice::LegalNotice(QDialog *parent)
    : QDialog(parent)
    , _ui(new Ui::LegalNotice)
{
    _ui->setupUi(this);

    connect(_ui->closeButton, &QPushButton::clicked, this, &LegalNotice::accept);

    if (!parent) {
        // Dialog visibility
        connect(this, &LegalNotice::onSetVisible, ownCloudGui::instance(), &ownCloudGui::slotDialogVisibilityChanged);
    }

    customizeStyle();
}

LegalNotice::~LegalNotice()
{
    delete _ui;
}

void LegalNotice::changeEvent(QEvent *e)
{
    switch (e->type()) {
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
    case QEvent::ThemeChange:
        customizeStyle();
        break;
    default:
        break;
    }

    QDialog::changeEvent(e);
}

void LegalNotice::customizeStyle()
{
    QString notice = tr("<p>Copyright 2017-2020 Nextcloud GmbH<br />"
                        "Copyright 2012-2018 ownCloud GmbH</p>");

    notice += tr("<p>Licensed under the GNU General Public License (GPL) Version 2.0 or any later version.</p>");

    notice += "<p>&nbsp;</p>";
    notice += Theme::instance()->aboutDetails();

    Theme::replaceLinkColorStringBackgroundAware(notice);

    _ui->notice->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextBrowserInteraction);
    _ui->notice->setText(notice);
    _ui->notice->setWordWrap(true);
    _ui->notice->setOpenExternalLinks(true);
}

void LegalNotice::setVisible(bool visible)
{
    emit onSetVisible(visible);
    QDialog::setVisible(visible);
}

} // namespace OCC
