/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#include "theme.h"
#include "config.h"
#include "common/utility.h"
#include "version.h"
#include "configfile.h"
#include "common/vfs.h"

#include <QtCore>
#ifndef TOKEN_AUTH_ONLY
#include <QtGui>
#include <QStyle>
#include <QApplication>
#endif
#include <QSslSocket>
#include <QSvgRenderer>

#include "nextcloudtheme.h"

#ifdef THEME_INCLUDE
#define Mirall OCC // namespace hack to make old themes work
#define QUOTEME(M) #M
#define INCLUDE_FILE(M) QUOTEME(M)
#include INCLUDE_FILE(THEME_INCLUDE)
#undef Mirall
#endif

namespace {

QUrl imagePathToUrl(const QString &imagePath)
{
    if (imagePath.startsWith(':')) {
        auto url = QUrl();
        url.setScheme(QStringLiteral("qrc"));
        url.setPath(imagePath.mid(1));
        return url;
    } else {
        return QUrl::fromLocalFile(imagePath);
    }
}

bool shouldPreferSvg()
{
    return QByteArray(APPLICATION_ICON_SET).toUpper() == QByteArrayLiteral("SVG");
}

}

namespace OCC {

Theme *Theme::_instance = nullptr;

Theme *Theme::instance()
{
    if (!_instance) {
        _instance = new THEME_CLASS;
        // some themes may not call the base ctor
        _instance->_mono = false;
    }
    return _instance;
}

Theme::~Theme() = default;

QString Theme::statusHeaderText(SyncResult::Status status) const
{
    QString resultStr;

    switch (status) {
    case SyncResult::Undefined:
        resultStr = QCoreApplication::translate("theme", "Status undefined");
        break;
    case SyncResult::NotYetStarted:
        resultStr = QCoreApplication::translate("theme", "Waiting to start sync");
        break;
    case SyncResult::SyncRunning:
        resultStr = QCoreApplication::translate("theme", "Sync is running");
        break;
    case SyncResult::Success:
        resultStr = QCoreApplication::translate("theme", "Sync Success");
        break;
    case SyncResult::Problem:
        resultStr = QCoreApplication::translate("theme", "Sync Success, some files were ignored.");
        break;
    case SyncResult::Error:
        resultStr = QCoreApplication::translate("theme", "Sync Error");
        break;
    case SyncResult::SetupError:
        resultStr = QCoreApplication::translate("theme", "Setup Error");
        break;
    case SyncResult::SyncPrepare:
        resultStr = QCoreApplication::translate("theme", "Preparing to sync");
        break;
    case SyncResult::SyncAbortRequested:
        resultStr = QCoreApplication::translate("theme", "Aborting …");
        break;
    case SyncResult::Paused:
        resultStr = QCoreApplication::translate("theme", "Sync is paused");
        break;
    }
    return resultStr;
}

bool Theme::isBranded() const
{
    return appNameGUI() != QStringLiteral("Nextcloud");
}

QString Theme::appNameGUI() const
{
    return APPLICATION_NAME;
}

QString Theme::appName() const
{
    return APPLICATION_SHORTNAME;
}

QString Theme::appRevDomain() const
{
    return APPLICATION_REV_DOMAIN;
}

QUrl Theme::stateOnlineImageSource() const
{
    return imagePathToUrl(themeImagePath("state-ok"));
}

QUrl Theme::stateOfflineImageSource() const
{
    return imagePathToUrl(themeImagePath("state-offline", 16));
}

QUrl Theme::statusOnlineImageSource() const
{
    return imagePathToUrl(themeImagePath("user-status-online", 16));
}

QUrl Theme::statusDoNotDisturbImageSource() const
{
    return imagePathToUrl(themeImagePath("user-status-dnd", 16));
}

QUrl Theme::statusAwayImageSource() const
{
    return imagePathToUrl(themeImagePath("user-status-away", 16));
}

QUrl Theme::statusInvisibleImageSource() const
{
    return imagePathToUrl(themeImagePath("user-status-invisible", 16));
}

QString Theme::version() const
{
    return MIRALL_VERSION_STRING;
}

QString Theme::configFileName() const
{
    return QStringLiteral(APPLICATION_EXECUTABLE ".cfg");
}

#ifndef TOKEN_AUTH_ONLY

QIcon Theme::applicationIcon() const
{
    return themeIcon(QStringLiteral(APPLICATION_ICON_NAME "-icon"));
}

/*
 * helper to load a icon from either the icon theme the desktop provides or from
 * the apps Qt resources.
 */
QIcon Theme::themeIcon(const QString &name, bool sysTray) const
{
    QString flavor;
    if (sysTray) {
        flavor = systrayIconFlavor(_mono);
    } else {
        flavor = QLatin1String("colored");
    }

    QString key = name + "," + flavor;
    QIcon &cached = _iconCache[key];
    if (cached.isNull()) {
        if (QIcon::hasThemeIcon(name)) {
            // use from theme
            return cached = QIcon::fromTheme(name);
        }

        const auto svgName = QString::fromLatin1(":/client/theme/%1/%2.svg").arg(flavor).arg(name);
        QSvgRenderer renderer(svgName);
        const auto createPixmapFromSvg = [&renderer] (int size) {
            QImage img(size, size, QImage::Format_ARGB32);
            img.fill(Qt::GlobalColor::transparent);
            QPainter imgPainter(&img);
            renderer.render(&imgPainter);
            return QPixmap::fromImage(img);
        };

        const auto loadPixmap = [flavor, name] (int size) {
            const auto pixmapName = QString::fromLatin1(":/client/theme/%1/%2-%3.png").arg(flavor).arg(name).arg(size);
            return QPixmap(pixmapName);
        };

        const auto useSvg = shouldPreferSvg();
        const auto sizes = useSvg ? QVector<int>{ 16, 32, 64, 128, 256 }
                                  : QVector<int>{ 16, 22, 32, 48, 64, 128, 256, 512, 1024 };
        for (int size : sizes) {
            auto px = useSvg ? createPixmapFromSvg(size) : loadPixmap(size);
            if (px.isNull()) {
                continue;
            }
            // HACK, get rid of it by supporting FDO icon themes, this is really just emulating ubuntu-mono
            if (qgetenv("DESKTOP_SESSION") == "ubuntu") {
                QBitmap mask = px.createMaskFromColor(Qt::white, Qt::MaskOutColor);
                QPainter p(&px);
                p.setPen(QColor("#dfdbd2"));
                p.drawPixmap(px.rect(), mask, mask.rect());
            }
            cached.addPixmap(px);
        }
    }

#ifdef Q_OS_MAC
#if QT_VERSION >= QT_VERSION_CHECK(5, 6, 0)
    // This defines the icon as a template and enables automatic macOS color handling
    // See https://bugreports.qt.io/browse/QTBUG-42109
    cached.setIsMask(_mono && sysTray);
#endif
#endif

    return cached;
}

QString Theme::themeImagePath(const QString &name, int size, bool sysTray) const
{
    const auto flavor = (!isBranded() && sysTray) ? systrayIconFlavor(_mono) : QLatin1String("colored");
    const auto useSvg = shouldPreferSvg();

    // branded client may have several sizes of the same icon
    const QString filePath = (useSvg || size <= 0)
            ? QString::fromLatin1(":/client/theme/%1/%2").arg(flavor).arg(name)
            : QString::fromLatin1(":/client/theme/%1/%2-%3").arg(flavor).arg(name).arg(size);

    const QString svgPath = filePath + ".svg";
    if (useSvg) {
        return svgPath;
    }

    const QString pngPath = filePath + ".png";
    // Use the SVG as fallback if a PNG is missing so that we get a chance to display something
    if (QFile::exists(pngPath)) {
        return pngPath;
    } else {
        return svgPath;
    }
}

bool Theme::isHidpi(QPaintDevice *dev)
{
    const auto devicePixelRatio = dev ? dev->devicePixelRatio() : qApp->primaryScreen()->devicePixelRatio();
    return devicePixelRatio > 1;
}

QIcon Theme::uiThemeIcon(const QString &iconName, bool uiHasDarkBg) const
{
    QString themeResBasePath = ":/client/theme/";
    QString iconPath = themeResBasePath + (uiHasDarkBg?"white/":"black/") + iconName;
    std::string icnPath = iconPath.toUtf8().constData();
    return QIcon(QPixmap(iconPath));
}

QString Theme::hidpiFileName(const QString &fileName, QPaintDevice *dev)
{
    if (!Theme::isHidpi(dev)) {
        return fileName;
    }
    // try to find a 2x version


    const int dotIndex = fileName.lastIndexOf(QLatin1Char('.'));
    if (dotIndex != -1) {
        QString at2xfileName = fileName;
        at2xfileName.insert(dotIndex, QStringLiteral("@2x"));
        if (QFile::exists(at2xfileName)) {
            return at2xfileName;
        }
    }
    return fileName;
}

QString Theme::hidpiFileName(const QString &iconName, const QColor &backgroundColor, QPaintDevice *dev)
{
    const auto isDarkBackground = Theme::isDarkColor(backgroundColor);

    const QString themeResBasePath = ":/client/theme/";
    const QString iconPath = themeResBasePath + (isDarkBackground ? "white/" : "black/") + iconName;

    return Theme::hidpiFileName(iconPath, dev);
}


#endif

Theme::Theme()
    : QObject(nullptr)
{
}

// If this option returns true, the client only supports one folder to sync.
// The Add-Button is removed accordingly.
bool Theme::singleSyncFolder() const
{
    return false;
}

bool Theme::multiAccount() const
{
    return true;
}

QString Theme::defaultServerFolder() const
{
    return QLatin1String("/");
}

QString Theme::helpUrl() const
{
#ifdef APPLICATION_HELP_URL
    return QString::fromLatin1(APPLICATION_HELP_URL);
#else
    return QString::fromLatin1("https://docs.nextcloud.com/desktop/%1.%2/").arg(MIRALL_VERSION_MAJOR).arg(MIRALL_VERSION_MINOR);
#endif
}

QString Theme::conflictHelpUrl() const
{
    auto baseUrl = helpUrl();
    if (baseUrl.isEmpty())
        return QString();
    if (!baseUrl.endsWith('/'))
        baseUrl.append('/');
    return baseUrl + QStringLiteral("conflicts.html");
}

QString Theme::overrideServerUrl() const
{
#ifdef APPLICATION_SERVER_URL
    return QString::fromLatin1(APPLICATION_SERVER_URL);
#else
    return QString();
#endif
}

bool Theme::forceOverrideServerUrl() const
{
#ifdef APPLICATION_SERVER_URL_ENFORCE
    return true;
#else
    return false;
#endif
}

QString Theme::forceConfigAuthType() const
{
    return QString();
}


QString Theme::defaultClientFolder() const
{
    return appName();
}

QString Theme::systrayIconFlavor(bool mono) const
{
    QString flavor;
    if (mono) {
        flavor = Utility::hasDarkSystray() ? QLatin1String("white") : QLatin1String("black");
    } else {
        flavor = QLatin1String("colored");
    }
    return flavor;
}

void Theme::setSystrayUseMonoIcons(bool mono)
{
    _mono = mono;
    emit systrayUseMonoIconsChanged(mono);
}

bool Theme::systrayUseMonoIcons() const
{
    return _mono;
}

bool Theme::monoIconsAvailable() const
{
    QString themeDir = QString::fromLatin1(":/client/theme/%1/").arg(Theme::instance()->systrayIconFlavor(true));
    return QDir(themeDir).exists();
}

QString Theme::updateCheckUrl() const
{
    return APPLICATION_UPDATE_URL;
}

qint64 Theme::newBigFolderSizeLimit() const
{
    // Default to 500MB
    return 500;
}

bool Theme::wizardHideExternalStorageConfirmationCheckbox() const
{
    return false;
}

bool Theme::wizardHideFolderSizeLimitCheckbox() const
{
    return false;
}

QString Theme::gitSHA1() const
{
    QString devString;
#ifdef GIT_SHA1
    const QString githubPrefix(QLatin1String(
        "https://github.com/nextcloud/desktop/commit/"));
    const QString gitSha1(QLatin1String(GIT_SHA1));
    devString = QCoreApplication::translate("nextcloudTheme::about()",
        "<p><small>Built from Git revision <a href=\"%1\">%2</a>"
        " on %3, %4 using Qt %5, %6</small></p>")
                    .arg(githubPrefix + gitSha1)
                    .arg(gitSha1.left(6))
                    .arg(__DATE__)
                    .arg(__TIME__)
                    .arg(qVersion())
                    .arg(QSslSocket::sslLibraryVersionString());
#endif
    return devString;
}

QString Theme::about() const
{
    // Shorten Qt's OS name: "macOS Mojave (10.14)" -> "macOS"
    QStringList osStringList = Utility::platformName().split(QLatin1Char(' '));
    QString osName = osStringList.at(0);

    QString devString;
    //: Example text: "<p>Nextcloud Desktop Client</p>"   (%1 is the application name)
    devString = tr("<p>%1 Desktop Client</p>")
              .arg(APPLICATION_NAME);

    devString += tr("<p>Version %1. For more information please click <a href='%2'>here</a>.</p>")
              .arg(QString::fromLatin1(MIRALL_STRINGIFY(MIRALL_VERSION)) + QString(" (%1)").arg(osName))
              .arg(helpUrl());

    devString += tr("<p><small>Using virtual files plugin: %1</small></p>")
                     .arg(Vfs::modeToString(bestAvailableVfsMode()));

    return devString;
}

QString Theme::aboutDetails() const
{
    QString devString;
    devString = tr("<p>Version %1. For more information please click <a href='%2'>here</a>.</p>")
              .arg(MIRALL_VERSION_STRING)
              .arg(helpUrl());

    devString += tr("<p>This release was supplied by %1</p>")
              .arg(APPLICATION_VENDOR);

    devString += gitSHA1();

    return devString;
}

#ifndef TOKEN_AUTH_ONLY
QVariant Theme::customMedia(CustomMediaType type)
{
    QVariant re;
    QString key;

    switch (type) {
    case oCSetupTop:
        key = QLatin1String("oCSetupTop");
        break;
    case oCSetupSide:
        key = QLatin1String("oCSetupSide");
        break;
    case oCSetupBottom:
        key = QLatin1String("oCSetupBottom");
        break;
    case oCSetupResultTop:
        key = QLatin1String("oCSetupResultTop");
        break;
    }

    QString imgPath = QString::fromLatin1(":/client/theme/colored/%1.png").arg(key);
    if (QFile::exists(imgPath)) {
        QPixmap pix(imgPath);
        if (pix.isNull()) {
            // pixmap loading hasn't succeeded. We take the text instead.
            re.setValue(key);
        } else {
            re.setValue(pix);
        }
    }
    return re;
}

QIcon Theme::syncStateIcon(SyncResult::Status status, bool sysTray) const
{
    // FIXME: Mind the size!
    QString statusIcon;

    switch (status) {
    case SyncResult::Undefined:
        // this can happen if no sync connections are configured.
        statusIcon = QLatin1String("state-warning");
        break;
    case SyncResult::NotYetStarted:
    case SyncResult::SyncRunning:
        statusIcon = QLatin1String("state-sync");
        break;
    case SyncResult::SyncAbortRequested:
    case SyncResult::Paused:
        statusIcon = QLatin1String("state-pause");
        break;
    case SyncResult::SyncPrepare:
    case SyncResult::Success:
        statusIcon = QLatin1String("state-ok");
        break;
    case SyncResult::Problem:
        statusIcon = QLatin1String("state-warning");
        break;
    case SyncResult::Error:
    case SyncResult::SetupError:
    // FIXME: Use state-problem once we have an icon.
    default:
        statusIcon = QLatin1String("state-error");
    }

    return themeIcon(statusIcon, sysTray);
}

QIcon Theme::folderDisabledIcon() const
{
    return themeIcon(QLatin1String("state-pause"));
}

QIcon Theme::folderOfflineIcon(bool sysTray) const
{
    return themeIcon(QLatin1String("state-offline"), sysTray);
}

QColor Theme::wizardHeaderTitleColor() const
{
    return {APPLICATION_WIZARD_HEADER_TITLE_COLOR};
}

QColor Theme::wizardHeaderBackgroundColor() const
{
    return {APPLICATION_WIZARD_HEADER_BACKGROUND_COLOR};
}

QPixmap Theme::wizardApplicationLogo() const
{
    if (!Theme::isBranded()) {
        return QPixmap(Theme::hidpiFileName(":/client/theme/colored/wizard-nextcloud.png"));
    }
#ifdef APPLICATION_WIZARD_USE_CUSTOM_LOGO
    const auto useSvg = shouldPreferSvg();
    const auto logoBasePath = QStringLiteral(":/client/theme/colored/wizard_logo");
    if (useSvg) {
        const auto maxHeight = Theme::isHidpi() ? 200 : 100;
        const auto maxWidth = 2 * maxHeight;
        const auto icon = QIcon(logoBasePath + ".svg");
        const auto size = icon.actualSize(QSize(maxWidth, maxHeight));
        return icon.pixmap(size);
    } else {
        return QPixmap(hidpiFileName(logoBasePath + ".png"));
    }
#else
    const auto size = Theme::isHidpi() ?: 200 : 100;
    return applicationIcon().pixmap(size);
#endif
}

QPixmap Theme::wizardHeaderLogo() const
{
#ifdef APPLICATION_WIZARD_USE_CUSTOM_LOGO
    const auto useSvg = shouldPreferSvg();
    const auto logoBasePath = QStringLiteral(":/client/theme/colored/wizard_logo");
    if (useSvg) {
        const auto maxHeight = 64;
        const auto maxWidth = 2 * maxHeight;
        const auto icon = QIcon(logoBasePath + ".svg");
        const auto size = icon.actualSize(QSize(maxWidth, maxHeight));
        return icon.pixmap(size);
    } else {
        return QPixmap(hidpiFileName(logoBasePath + ".png"));
    }
#else
    return applicationIcon().pixmap(64);
#endif
}

QPixmap Theme::wizardHeaderBanner() const
{
    QColor c = wizardHeaderBackgroundColor();
    if (!c.isValid())
        return QPixmap();

    QSize size(750, 78);
    if (auto screen = qApp->primaryScreen()) {
        // Adjust the the size if there is a different DPI. (Issue #6156)
        // Indeed, this size need to be big enough to for the banner height, and the wizard's width
        auto ratio = screen->logicalDotsPerInch() / 96.;
        if (ratio > 1.)
            size *= ratio;
    }
    QPixmap pix(size);
    pix.fill(wizardHeaderBackgroundColor());
    return pix;
}
#endif

bool Theme::wizardSelectiveSyncDefaultNothing() const
{
    return false;
}

QString Theme::webDavPath() const
{
    return QLatin1String("remote.php/webdav/");
}

QString Theme::webDavPathNonShib() const
{
    return QLatin1String("remote.php/nonshib-webdav/");
}

bool Theme::linkSharing() const
{
    return true;
}

bool Theme::userGroupSharing() const
{
    return true;
}

bool Theme::forceSystemNetworkProxy() const
{
    return false;
}

Theme::UserIDType Theme::userIDType() const
{
    return UserIDType::UserIDUserName;
}

QString Theme::customUserID() const
{
    return QString();
}

QString Theme::userIDHint() const
{
    return QString();
}


QString Theme::wizardUrlPostfix() const
{
    return QString();
}

QString Theme::wizardUrlHint() const
{
    return QString();
}

QString Theme::quotaBaseFolder() const
{
    return QLatin1String("/");
}

QString Theme::oauthClientId() const
{
    return "xdXOt13JKxym1B1QcEncf2XDkLAexMBFwiT9j6EfhhHFJhs2KM9jbjTmf8JBXE69";
}

QString Theme::oauthClientSecret() const
{
    return "UBntmLjC2yYCeHwsyj73Uwo9TAaecAetRwMw0xYcvNL9yRdLSUi0hUAHfvCHFeFh";
}

QString Theme::versionSwitchOutput() const
{
    QString helpText;
    QTextStream stream(&helpText);
    stream << appName()
           << QLatin1String(" version ")
           << version() << endl;
#ifdef GIT_SHA1
    stream << "Git revision " << GIT_SHA1 << endl;
#endif
    stream << "Using Qt " << qVersion() << ", built against Qt " << QT_VERSION_STR << endl;

    if(!QGuiApplication::platformName().isEmpty())
        stream << "Using Qt platform plugin '" << QGuiApplication::platformName() << "'" << endl;

    stream << "Using '" << QSslSocket::sslLibraryVersionString() << "'" << endl;
    stream << "Running on " << Utility::platformName() << ", " << QSysInfo::currentCpuArchitecture() << endl;
    return helpText;
}

bool Theme::isDarkColor(const QColor &color)
{
    // account for different sensitivity of the human eye to certain colors
    double treshold = 1.0 - (0.299 * color.red() + 0.587 * color.green() + 0.114 * color.blue()) / 255.0;
    return treshold > 0.5;
}

QColor Theme::getBackgroundAwareLinkColor(const QColor &backgroundColor)
{
    return {(isDarkColor(backgroundColor) ? QColor("#6193dc") : QGuiApplication::palette().color(QPalette::Link))};
}

QColor Theme::getBackgroundAwareLinkColor()
{
    return getBackgroundAwareLinkColor(QGuiApplication::palette().base().color());
}

void Theme::replaceLinkColorStringBackgroundAware(QString &linkString, const QColor &backgroundColor)
{
    replaceLinkColorString(linkString, getBackgroundAwareLinkColor(backgroundColor));
}

void Theme::replaceLinkColorStringBackgroundAware(QString &linkString)
{
    replaceLinkColorStringBackgroundAware(linkString, QGuiApplication::palette().color(QPalette::Base));
}

void Theme::replaceLinkColorString(QString &linkString, const QColor &newColor)
{
    linkString.replace(QRegularExpression("(<a href|<a style='color:#([a-zA-Z0-9]{6});' href)"), QString::fromLatin1("<a style='color:%1;' href").arg(newColor.name()));
}

QIcon Theme::createColorAwareIcon(const QString &name, const QPalette &palette)
{
    QSvgRenderer renderer(name);
    QImage img(64, 64, QImage::Format_ARGB32);
    img.fill(Qt::GlobalColor::transparent);
    QPainter imgPainter(&img);
    QImage inverted(64, 64, QImage::Format_ARGB32);
    inverted.fill(Qt::GlobalColor::transparent);
    QPainter invPainter(&inverted);

    renderer.render(&imgPainter);
    renderer.render(&invPainter);

    inverted.invertPixels(QImage::InvertRgb);

    QIcon icon;
    if (Theme::isDarkColor(palette.color(QPalette::Base))) {
        icon.addPixmap(QPixmap::fromImage(inverted));
    } else {
        icon.addPixmap(QPixmap::fromImage(img));
    }
    if (Theme::isDarkColor(palette.color(QPalette::HighlightedText))) {
        icon.addPixmap(QPixmap::fromImage(img), QIcon::Normal, QIcon::On);
    } else {
        icon.addPixmap(QPixmap::fromImage(inverted), QIcon::Normal, QIcon::On);
    }
    return icon;
}

QIcon Theme::createColorAwareIcon(const QString &name)
{
    return createColorAwareIcon(name, QGuiApplication::palette());
}

QPixmap Theme::createColorAwarePixmap(const QString &name, const QPalette &palette)
{
    QImage img(name);
    QImage inverted(img);
    inverted.invertPixels(QImage::InvertRgb);

    QPixmap pixmap;
    if (Theme::isDarkColor(palette.color(QPalette::Base))) {
        pixmap = QPixmap::fromImage(inverted);
    } else {
        pixmap = QPixmap::fromImage(img);
    }
    return pixmap;
}

QPixmap Theme::createColorAwarePixmap(const QString &name)
{
    return createColorAwarePixmap(name, QGuiApplication::palette());
}

bool Theme::showVirtualFilesOption() const
{
    const auto vfsMode = bestAvailableVfsMode();
    return ConfigFile().showExperimentalOptions() || vfsMode == Vfs::WindowsCfApi;
}

} // end namespace client
