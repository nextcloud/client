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

#ifndef OCC_PROXYAUTHDIALOG_H
#define OCC_PROXYAUTHDIALOG_H

#include <QDialog>

namespace OCC {

namespace Ui {
    class ProxyAuthDialog;
}

/**
 * @brief Ask for username and password for a given proxy.
 *
 * Used by ProxyAuthHandler.
 */
class ProxyAuthDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProxyAuthDialog(QWidget *parent = nullptr);
    ~ProxyAuthDialog();

    void setProxyAddress(const QString &address);

    QString username() const;
    QString password() const;

    /// Resets the dialog for new credential entry.
    void reset();

signals:
    void onSetVisible(bool visible);

protected:
    void setVisible(bool visible) override;

private:
    Ui::ProxyAuthDialog *ui;
};


} // namespace OCC
#endif // OCC_PROXYAUTHDIALOG_H
