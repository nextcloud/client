/*
 * Copyright (C) by Klaas Freitag <freitag@kde.org>
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
#include "sslerrordialog.h"
#include "owncloudgui.h"

#include <QtGui>
#include <QtNetwork>
#include <QtWidgets>


#include "ui_sslerrordialog.h"

namespace OCC {

Q_LOGGING_CATEGORY(lcSslErrorDialog, "nextcloud.gui.sslerrordialog", QtInfoMsg)

namespace Utility {
    //  Used for QSSLCertificate::subjectInfo which returns a QStringList in Qt5, but a QString in Qt4
    QString escape(const QStringList &l) { return escape(l.join(';')); }
}

bool SslDialogErrorHandler::handleErrors(QList<QSslError> errors, const QSslConfiguration &conf, QList<QSslCertificate> *certs, AccountPtr account)
{
    (void)conf;
    if (!certs) {
        qCCritical(lcSslErrorDialog) << "Certs parameter required but is NULL!";
        return false;
    }

    SslErrorDialog dlg(account);
    // whether the failing certs have previously been accepted
    if (dlg.checkFailingCertsKnown(errors)) {
        *certs = dlg.unknownCerts();
        return true;
    }
    // whether the user accepted the certs
    if (dlg.exec() == QDialog::Accepted) {
        if (dlg.trustConnection()) {
            *certs = dlg.unknownCerts();
            return true;
        }
    }
    return false;
}

SslErrorDialog::SslErrorDialog(AccountPtr account, QWidget *parent)
    : QDialog(parent)
    , _allTrusted(false)
    , _ui(new Ui::SslErrorDialog)
    , _account(account)
{
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    _ui->setupUi(this);
    setWindowTitle(tr("Untrusted Certificate"));
    QPushButton *okButton =
        _ui->_dialogButtonBox->button(QDialogButtonBox::Ok);
    QPushButton *cancelButton =
        _ui->_dialogButtonBox->button(QDialogButtonBox::Cancel);
    okButton->setEnabled(false);
    connect(_ui->_cbTrustConnect, &QAbstractButton::clicked,
        okButton, &QWidget::setEnabled);

    if (okButton) {
        okButton->setDefault(true);
        connect(okButton, &QAbstractButton::clicked, this, &QDialog::accept);
        connect(cancelButton, &QAbstractButton::clicked, this, &QDialog::reject);
    }

    if (!parent) {
        // Dialog visibility
        connect(this, &SslErrorDialog::onSetVisible, ownCloudGui::instance(), &ownCloudGui::slotDialogVisibilityChanged);
    }
}

SslErrorDialog::~SslErrorDialog()
{
    delete _ui;
}


QString SslErrorDialog::styleSheet() const
{
    const QString style = QLatin1String(
        "#cert {margin-left: 5px;} "
        "#ca_error { color:#a00011; margin-left:5px; margin-right:5px; }"
        "#ca_error p { margin-top: 2px; margin-bottom:2px; }"
        "#ccert { margin-left: 5px; }"
        "#issuer { margin-left: 5px; }"
        "tt { font-size: small; }");

    return style;
}
#define QL(x) QLatin1String(x)

bool SslErrorDialog::checkFailingCertsKnown(const QList<QSslError> &errors)
{
    // check if unknown certs caused errors.
    _unknownCerts.clear();

    QStringList errorStrings;

    QList<QSslCertificate> trustedCerts = _account->approvedCerts();

    for (int i = 0; i < errors.count(); ++i) {
        QSslError error = errors.at(i);
        if (trustedCerts.contains(error.certificate()) || _unknownCerts.contains(error.certificate())) {
            continue;
        }
        errorStrings += error.errorString();
        if (!error.certificate().isNull()) {
            _unknownCerts.append(error.certificate());
        }
    }

    // if there are no errors left, all Certs were known.
    if (errorStrings.isEmpty()) {
        _allTrusted = true;
        return true;
    }

    QString msg = QL("<html><head>");
    msg += QL("<link rel='stylesheet' type='text/css' href='format.css'>");
    msg += QL("</head><body>");

    auto host = _account->url().host();
    msg += QL("<h3>") + tr("Cannot connect securely to <i>%1</i>:").arg(host) + QL("</h3>");
    // loop over the unknown certs and line up their errors.
    msg += QL("<div id=\"ca_errors\">");
    foreach (const QSslCertificate &cert, _unknownCerts) {
        msg += QL("<div id=\"ca_error\">");
        // add the errors for this cert
        foreach (QSslError err, errors) {
            if (err.certificate() == cert) {
                msg += QL("<p>") + err.errorString() + QL("</p>");
            }
        }
        msg += QL("</div>");
        msg += certDiv(cert);
        if (_unknownCerts.count() > 1) {
            msg += QL("<hr/>");
        }
    }
    msg += QL("</div></body></html>");

    auto *doc = new QTextDocument(nullptr);
    QString style = styleSheet();
    doc->addResource(QTextDocument::StyleSheetResource, QUrl(QL("format.css")), style);
    doc->setHtml(msg);

    _ui->_tbErrors->setDocument(doc);
    _ui->_tbErrors->show();

    return false;
}

QString SslErrorDialog::certDiv(QSslCertificate cert) const
{
    QString msg;
    msg += QL("<div id=\"cert\">");
    msg += QL("<h3>") + tr("with Certificate %1").arg(Utility::escape(cert.subjectInfo(QSslCertificate::CommonName))) + QL("</h3>");

    msg += QL("<div id=\"ccert\">");
    QStringList li;

    QString org = Utility::escape(cert.subjectInfo(QSslCertificate::Organization));
    QString unit = Utility::escape(cert.subjectInfo(QSslCertificate::OrganizationalUnitName));
    QString country = Utility::escape(cert.subjectInfo(QSslCertificate::CountryName));
    if (unit.isEmpty())
        unit = tr("&lt;not specified&gt;");
    if (org.isEmpty())
        org = tr("&lt;not specified&gt;");
    if (country.isEmpty())
        country = tr("&lt;not specified&gt;");
    li << tr("Organization: %1").arg(org);
    li << tr("Unit: %1").arg(unit);
    li << tr("Country: %1").arg(country);
    msg += QL("<p>") + li.join(QL("<br/>")) + QL("</p>");

    msg += QL("<p>");

    if (cert.effectiveDate() < QDateTime(QDate(2016, 1, 1), QTime(), Qt::UTC)) {
	QString sha1sum = Utility::formatFingerprint(cert.digest(QCryptographicHash::Sha1).toHex());
        msg += tr("Fingerprint (SHA1): <tt>%1</tt>").arg(sha1sum) + QL("<br/>");
    }

    QString sha256sum = Utility::formatFingerprint(cert.digest(QCryptographicHash::Sha256).toHex());
    QString sha512sum = Utility::formatFingerprint(cert.digest(QCryptographicHash::Sha512).toHex());
    msg += tr("Fingerprint (SHA-256): <tt>%1</tt>").arg(sha256sum) + QL("<br/>");
    msg += tr("Fingerprint (SHA-512): <tt>%1</tt>").arg(sha512sum) + QL("<br/>");
    msg += QL("<br/>");
    msg += tr("Effective Date: %1").arg(cert.effectiveDate().toString()) + QL("<br/>");
    msg += tr("Expiration Date: %1").arg(cert.expiryDate().toString()) + QL("</p>");

    msg += QL("</div>");

    msg += QL("<h3>") + tr("Issuer: %1").arg(Utility::escape(cert.issuerInfo(QSslCertificate::CommonName))) + QL("</h3>");
    msg += QL("<div id=\"issuer\">");
    li.clear();
    li << tr("Organization: %1").arg(Utility::escape(cert.issuerInfo(QSslCertificate::Organization)));
    li << tr("Unit: %1").arg(Utility::escape(cert.issuerInfo(QSslCertificate::OrganizationalUnitName)));
    li << tr("Country: %1").arg(Utility::escape(cert.issuerInfo(QSslCertificate::CountryName)));
    msg += QL("<p>") + li.join(QL("<br/>")) + QL("</p>");
    msg += QL("</div>");
    msg += QL("</div>");

    return msg;
}

bool SslErrorDialog::trustConnection()
{
    if (_allTrusted)
        return true;

    bool stat = (_ui->_cbTrustConnect->checkState() == Qt::Checked);
    qCInfo(lcSslErrorDialog) << "SSL-Connection is trusted: " << stat;

    return stat;
}

void SslErrorDialog::setVisible(bool visible)
{
    emit onSetVisible(visible);
    QDialog::setVisible(visible);
}

} // end namespace
