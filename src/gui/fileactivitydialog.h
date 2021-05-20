/*
 * Copyright (C) by Felix Weilbach <felix.weilbach@nextcloud.com>
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

#include <QDialog>

#include "fileactivitymodel.h"
#include "account.h"

namespace OCC {

namespace Ui {
    class FileActivityDialog;
}

class FileActivityDialog : public QDialog
{
public:
    explicit FileActivityDialog(AccountPtr account, const QString &fileId, QWidget *parent = nullptr);

private:
    void onShowActivities();
    void onHideActivities();
    void onShowError(const QString &message);
    void onHideError();
    void onShowProgress();
    void onHideProgress();

    Ui::FileActivityDialog *_ui;
    FileActivityDialogModel _model;
};
}
