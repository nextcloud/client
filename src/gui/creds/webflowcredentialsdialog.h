#ifndef WEBFLOWCREDENTIALSDIALOG_H
#define WEBFLOWCREDENTIALSDIALOG_H

#include <QDialog>
#include <QUrl>
#include <QTimer>

#include "accountfwd.h"
#include "creds/flow2auth.h"

class QLabel;
class QVBoxLayout;

namespace OCC {

class HeaderBanner;
class WebView;
class Flow2AuthWidget;

class WebFlowCredentialsDialog : public QDialog
{
    Q_OBJECT
public:
    WebFlowCredentialsDialog(Account *account, bool useFlow2, QWidget *parent = nullptr);

    void setUrl(const QUrl &url);
    void setInfo(const QString &msg);
    void setError(const QString &error);

    bool isUsingFlow2() const {
        return _useFlow2;
    }

protected:
    void closeEvent(QCloseEvent * e) override;
    void changeEvent(QEvent *) override;

public slots:
    void slotFlow2AuthResult(Flow2Auth::Result, const QString &errorString, const QString &user, const QString &appPassword);
    void slotShowSettingsDialog();

signals:
    void urlCatched(const QString user, const QString pass, const QString host);
    void styleChanged();
    void onActivate();
    void onClose();

private:
    void customizeStyle();

    bool _useFlow2;

    Flow2AuthWidget *_flow2AuthWidget;
    WebView *_webView;

    QLabel *_errorLabel;
    QLabel *_infoLabel;
    QVBoxLayout *_layout;
    QVBoxLayout *_containerLayout;
    HeaderBanner *_headerBanner;

    QTimer _raiseDelayTimer;
};

} // namespace OCC

#endif // WEBFLOWCREDENTIALSDIALOG_H
