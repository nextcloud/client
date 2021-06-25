#include "webflowcredentialsdialog.h"

#include <QVBoxLayout>
#include <QLabel>

#include "theme.h"
#include "application.h"
#include "owncloudgui.h"
#include "headerbanner.h"
#include "wizard/owncloudwizardcommon.h"
#ifdef WITH_WEBENGINE
#include "wizard/webview.h"
#endif // WITH_WEBENGINE
#include "wizard/flow2authwidget.h"

namespace OCC {

WebFlowCredentialsDialog::WebFlowCredentialsDialog(Account *account, bool useFlow2, QWidget *parent)
    : QDialog(parent)
    , _useFlow2(useFlow2)
    , _flow2AuthWidget(nullptr)
#ifdef WITH_WEBENGINE
    , _webView(nullptr)
#endif // WITH_WEBENGINE
{
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);

    _layout = new QVBoxLayout(this);
    int spacing = _layout->spacing();
    int margin = _layout->margin();
    _layout->setSpacing(0);
    _layout->setMargin(0);

    _containerLayout = new QVBoxLayout(this);
    _containerLayout->setSpacing(spacing);
    _containerLayout->setMargin(margin);

    _infoLabel = new QLabel();
    _infoLabel->setAlignment(Qt::AlignCenter);
    _containerLayout->addWidget(_infoLabel);

    if (_useFlow2) {
        _flow2AuthWidget = new Flow2AuthWidget();
        _containerLayout->addWidget(_flow2AuthWidget);

        connect(_flow2AuthWidget, &Flow2AuthWidget::authResult, this, &WebFlowCredentialsDialog::slotFlow2AuthResult);

        // Connect styleChanged events to our widgets, so they can adapt (Dark-/Light-Mode switching)
        connect(this, &WebFlowCredentialsDialog::styleChanged, _flow2AuthWidget, &Flow2AuthWidget::slotStyleChanged);

        // allow Flow2 page to poll on window activation
        connect(this, &WebFlowCredentialsDialog::onActivate, _flow2AuthWidget, &Flow2AuthWidget::slotPollNow);

        _flow2AuthWidget->startAuth(account);
    } else {
#ifdef WITH_WEBENGINE
        _webView = new WebView();
        _containerLayout->addWidget(_webView);

        connect(_webView, &WebView::urlCatched, this, &WebFlowCredentialsDialog::urlCatched);
#endif // WITH_WEBENGINE
    }

    auto app = static_cast<Application *>(qApp);
    connect(app, &Application::isShowingSettingsDialog, this, &WebFlowCredentialsDialog::slotShowSettingsDialog);

    _errorLabel = new QLabel();
    _errorLabel->hide();
    _containerLayout->addWidget(_errorLabel);

    WizardCommon::initErrorLabel(_errorLabel);

    _layout->addLayout(_containerLayout);
    setLayout(_layout);

    customizeStyle();
}

void WebFlowCredentialsDialog::closeEvent(QCloseEvent* e) {
    Q_UNUSED(e)

#ifdef WITH_WEBENGINE
    if (_webView) {
        // Force calling WebView::~WebView() earlier so that _profile and _page are
        // deleted in the correct order.
        _webView->deleteLater();
        _webView = nullptr;
    }
#endif // WITH_WEBENGINE

    if (_flow2AuthWidget) {
        _flow2AuthWidget->resetAuth();
        _flow2AuthWidget->deleteLater();
        _flow2AuthWidget = nullptr;
    }

    emit onClose();
}

void WebFlowCredentialsDialog::setUrl(const QUrl &url)
{
#ifdef WITH_WEBENGINE
    if (_webView)
        _webView->setUrl(url);
#else // WITH_WEBENGINE
    Q_UNUSED(url);
#endif // WITH_WEBENGINE
}

void WebFlowCredentialsDialog::setInfo(const QString &msg) {
    _infoLabel->setText(msg);
}

void WebFlowCredentialsDialog::setError(const QString &error) {
    // bring window to top
    slotShowSettingsDialog();

    if (_useFlow2 && _flow2AuthWidget) {
        _flow2AuthWidget->setError(error);
        return;
    }

    if (error.isEmpty()) {
        _errorLabel->hide();
    } else {
        _errorLabel->setText(error);
        _errorLabel->show();
    }
}

void WebFlowCredentialsDialog::changeEvent(QEvent *e)
{
    switch (e->type()) {
    case QEvent::StyleChange:
    case QEvent::PaletteChange:
    case QEvent::ThemeChange:
        customizeStyle();

        // Notify the other widgets (Dark-/Light-Mode switching)
        emit styleChanged();
        break;
    case QEvent::ActivationChange:
        if(isActiveWindow())
            emit onActivate();
        break;
    default:
        break;
    }

    QDialog::changeEvent(e);
}

void WebFlowCredentialsDialog::customizeStyle()
{
    // HINT: Customize dialog's own style here, if necessary in the future (Dark-/Light-Mode switching)
}

void WebFlowCredentialsDialog::slotShowSettingsDialog()
{
    // bring window to top but slightly delay, to avoid being hidden behind the SettingsDialog
    QTimer::singleShot(100, this, [this] {
        ownCloudGui::raiseDialog(this);
    });
}

void WebFlowCredentialsDialog::slotFlow2AuthResult(Flow2Auth::Result r, const QString &errorString, const QString &user, const QString &appPassword)
{
    Q_UNUSED(errorString)
    if(r == Flow2Auth::LoggedIn) {
        emit urlCatched(user, appPassword, QString());
    } else {
        // bring window to top
        slotShowSettingsDialog();
    }
}

} // namespace OCC
