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

#ifndef LEGALNOTICE_H
#define LEGALNOTICE_H

#include <QDialog>

namespace OCC {
class IgnoreListEditor;
class SyncLogDialog;

namespace Ui {
    class LegalNotice;
}

/**
 * @brief The LegalNotice class
 * @ingroup gui
 */
class LegalNotice : public QDialog
{
    Q_OBJECT

public:
    explicit LegalNotice(QDialog *parent = nullptr);
    ~LegalNotice();

signals:
    void onSetVisible(bool visible);

protected:
    void changeEvent(QEvent *) override;
    void setVisible(bool visible) override;

private:
    void customizeStyle();

    Ui::LegalNotice *_ui;
};

} // namespace OCC
#endif // LEGALNOTICE_H
