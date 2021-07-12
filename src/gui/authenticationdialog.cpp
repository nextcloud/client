/*
 * Copyright (C) 2014 by Daniel Molkentin <danimo@owncloud.com>
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

#include "authenticationdialog.h"

#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QDialogButtonBox>

namespace OCC {

AuthenticationDialog::AuthenticationDialog(const QString &realm, const QString &domain, QWidget *parent)
    : QDialog(parent)
    , _user(new QLineEdit)
    , _password(new QLineEdit)
{
    setWindowTitle(tr("Authentication Required"));
    auto *lay = new QVBoxLayout(this);
    auto *label = new QLabel(tr("Enter username and password for \"%1\" at %2.").arg(realm, domain));
    label->setTextFormat(Qt::PlainText);
    lay->addWidget(label);

    auto *form = new QFormLayout;
    form->addRow(tr("&User:"), _user);
    form->addRow(tr("&Password:"), _password);
    lay->addLayout(form);
    _password->setEchoMode(QLineEdit::Password);

    auto *box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, Qt::Horizontal);
    connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    lay->addWidget(box);
}

QString AuthenticationDialog::user() const
{
    return _user->text();
}

QString AuthenticationDialog::password() const
{
    return _password->text();
}

} // namespace OCC
