/*
 * Copyright (C) by Daniel Molkentin <danimo@owncloud.com>
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

#include "configfile.h"

#include "folderman.h"
#include "generalsettings.h"
#include "ignorelisteditor.h"
#include "ui_ignorelisteditor.h"

#include <QFile>
#include <QDir>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QInputDialog>

namespace OCC {

IgnoreListEditor::IgnoreListEditor(QWidget *parent)
    : QDialog(parent)
    , ui(new Ui::IgnoreListEditor)
{
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    ui->setupUi(this);

    ConfigFile cfgFile;
    //FIXME This is not true. The entries are hardcoded below in setupTableReadOnlyItems
    readOnlyTooltip = tr("This entry is provided by the system at \"%1\" "
                         "and cannot be modified in this view.")
                          .arg(QDir::toNativeSeparators(cfgFile.excludeFile(ConfigFile::SystemScope)));

    setupTableReadOnlyItems();
    const auto userConfig = cfgFile.excludeFile(ConfigFile::Scope::UserScope);
    ui->ignoreTableWidget->readIgnoreFile(userConfig);

    connect(this, &QDialog::accepted, [=]() {
        ui->ignoreTableWidget->slotWriteIgnoreFile(userConfig);
        /* handle the hidden file checkbox */

        /* the ignoreHiddenFiles flag is a folder specific setting, but for now, it is
        * handled globally. Save it to every folder that is defined.
        * TODO this can now be fixed, simply attach this IgnoreListEditor to top-level account
        * settings
        */
        FolderMan::instance()->setIgnoreHiddenFiles(ignoreHiddenFiles());
    });
    connect(ui->buttonBox, &QDialogButtonBox::clicked,
            this, &IgnoreListEditor::slotRestoreDefaults);

    ui->syncHiddenFilesCheckBox->setChecked(!FolderMan::instance()->ignoreHiddenFiles());
}

IgnoreListEditor::~IgnoreListEditor()
{
    delete ui;
}

void IgnoreListEditor::setupTableReadOnlyItems()
{
    ui->ignoreTableWidget->addPattern(".csync_journal.db*", /*deletable=*/false, /*readonly=*/true);
    ui->ignoreTableWidget->addPattern("._sync_*.db*", /*deletable=*/false, /*readonly=*/true);
    ui->ignoreTableWidget->addPattern(".sync_*.db*", /*deletable=*/false, /*readonly=*/true);
}

bool IgnoreListEditor::ignoreHiddenFiles()
{
    return !ui->syncHiddenFilesCheckBox->isChecked();
}

void IgnoreListEditor::slotRestoreDefaults(QAbstractButton *button)
{
    if(ui->buttonBox->buttonRole(button) != QDialogButtonBox::ResetRole)
        return;

    ui->ignoreTableWidget->slotRemoveAllItems();

    ConfigFile cfgFile;
    setupTableReadOnlyItems();
    ui->ignoreTableWidget->readIgnoreFile(cfgFile.excludeFile(ConfigFile::SystemScope), false);
}

} // namespace OCC
