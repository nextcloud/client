/*
 * Copyright (C) 2015 by Christian Kamm <kamm@incasoftware.de>
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

#include "proxyauthdialog.h"
#include "ui_proxyauthdialog.h"

#include "owncloudgui.h"

namespace OCC {

ProxyAuthDialog::ProxyAuthDialog(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::ProxyAuthDialog)
{
    ui->setupUi(this);

    if (!parent) {
        // Dialog visibility
        connect(this, &ProxyAuthDialog::onSetVisible, ownCloudGui::instance(), &ownCloudGui::slotDialogVisibilityChanged);
    }
}

ProxyAuthDialog::~ProxyAuthDialog()
{
    delete ui;
}

void ProxyAuthDialog::setProxyAddress(const QString &address)
{
    ui->proxyAddress->setText(address);
}

QString ProxyAuthDialog::username() const
{
    return ui->usernameEdit->text();
}

QString ProxyAuthDialog::password() const
{
    return ui->passwordEdit->text();
}

void ProxyAuthDialog::reset()
{
    ui->usernameEdit->setFocus();
    ui->usernameEdit->clear();
    ui->passwordEdit->clear();
}

void ProxyAuthDialog::setVisible(bool visible)
{
    emit onSetVisible(visible);
    QDialog::setVisible(visible);
}

} // namespace OCC
