/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
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

#include <QJsonDocument>
#include <QLoggingCategory>
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QSslCipher>
#include <QBuffer>
#include <QXmlStreamReader>
#include <QStringList>
#include <QStack>
#include <QTimer>
#include <QMutex>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#ifndef TOKEN_AUTH_ONLY
#include <QPainter>
#include <QPainterPath>
#endif

#include "networkjobs.h"
#include "account.h"
#include "owncloudpropagator.h"
#include "clientsideencryption.h"

#include "creds/abstractcredentials.h"
#include "creds/httpcredentials.h"

namespace OCC {

Q_LOGGING_CATEGORY(lcEtagJob, "nextcloud.sync.networkjob.etag", QtInfoMsg)
Q_LOGGING_CATEGORY(lcLsColJob, "nextcloud.sync.networkjob.lscol", QtInfoMsg)
Q_LOGGING_CATEGORY(lcCheckServerJob, "nextcloud.sync.networkjob.checkserver", QtInfoMsg)
Q_LOGGING_CATEGORY(lcPropfindJob, "nextcloud.sync.networkjob.propfind", QtInfoMsg)
Q_LOGGING_CATEGORY(lcAvatarJob, "nextcloud.sync.networkjob.avatar", QtInfoMsg)
Q_LOGGING_CATEGORY(lcMkColJob, "nextcloud.sync.networkjob.mkcol", QtInfoMsg)
Q_LOGGING_CATEGORY(lcProppatchJob, "nextcloud.sync.networkjob.proppatch", QtInfoMsg)
Q_LOGGING_CATEGORY(lcJsonApiJob, "nextcloud.sync.networkjob.jsonapi", QtInfoMsg)
Q_LOGGING_CATEGORY(lcDetermineAuthTypeJob, "nextcloud.sync.networkjob.determineauthtype", QtInfoMsg)
const int notModifiedStatusCode = 304;

QByteArray parseEtag(const char *header)
{
    if (!header)
        return QByteArray();
    QByteArray arr = header;

    // Weak E-Tags can appear when gzip compression is on, see #3946
    if (arr.startsWith("W/"))
        arr = arr.mid(2);

    // https://github.com/owncloud/client/issues/1195
    arr.replace("-gzip", "");

    if (arr.length() >= 2 && arr.startsWith('"') && arr.endsWith('"')) {
        arr = arr.mid(1, arr.length() - 2);
    }
    return arr;
}

RequestEtagJob::RequestEtagJob(AccountPtr account, const QString &path, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
{
}

void RequestEtagJob::start()
{
    QNetworkRequest req;
    req.setRawHeader("Depth", "0");

    QByteArray xml("<?xml version=\"1.0\" ?>\n"
                   "<d:propfind xmlns:d=\"DAV:\">\n"
                   "  <d:prop>\n"
                   "    <d:getetag/>\n"
                   "  </d:prop>\n"
                   "</d:propfind>\n");
    auto *buf = new QBuffer(this);
    buf->setData(xml);
    buf->open(QIODevice::ReadOnly);
    // assumes ownership
    sendRequest("PROPFIND", makeDavUrl(path()), req, buf);
    AbstractNetworkJob::start();
}

bool RequestEtagJob::finished()
{
    qCInfo(lcEtagJob) << "Request Etag of" << reply()->request().url() << "FINISHED WITH STATUS"
                      <<  replyStatusString();

    auto httpCode = reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpCode == 207) {
        // Parse DAV response
        QXmlStreamReader reader(reply());
        reader.addExtraNamespaceDeclaration(QXmlStreamNamespaceDeclaration("d", "DAV:"));
        QString etag;
        while (!reader.atEnd()) {
            QXmlStreamReader::TokenType type = reader.readNext();
            if (type == QXmlStreamReader::StartElement && reader.namespaceUri() == QLatin1String("DAV:")) {
                QString name = reader.name().toString();
                if (name == QLatin1String("getetag")) {
                    auto etagText = reader.readElementText();
                    auto parsedTag = parseEtag(etagText.toUtf8());
                    if (!parsedTag.isEmpty()) {
                        etag += QString::fromUtf8(parsedTag);
                    } else {
                        etag += etagText;
                    }
                }
            }
        }
        emit etagRetrieved(etag, QDateTime::fromString(QString::fromUtf8(_responseTimestamp), Qt::RFC2822Date));
        emit finishedWithResult(etag);
    } else {
        emit finishedWithResult(HttpError{ httpCode, errorString() });
    }
    return true;
}

/*********************************************************************************************/

MkColJob::MkColJob(AccountPtr account, const QString &path, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
{
}

MkColJob::MkColJob(AccountPtr account, const QString &path, const QMap<QByteArray, QByteArray> &extraHeaders, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
    , _extraHeaders(extraHeaders)
{
}

MkColJob::MkColJob(AccountPtr account, const QUrl &url,
    const QMap<QByteArray, QByteArray> &extraHeaders, QObject *parent)
    : AbstractNetworkJob(account, QString(), parent)
    , _url(url)
    , _extraHeaders(extraHeaders)
{
}

void MkColJob::start()
{
    // add 'Content-Length: 0' header (see https://github.com/owncloud/client/issues/3256)
    QNetworkRequest req;
    req.setRawHeader("Content-Length", "0");
    for (auto it = _extraHeaders.constBegin(); it != _extraHeaders.constEnd(); ++it) {
        req.setRawHeader(it.key(), it.value());
    }

    // assumes ownership
    if (_url.isValid()) {
        sendRequest("MKCOL", _url, req);
    } else {
        sendRequest("MKCOL", makeDavUrl(path()), req);
    }
    AbstractNetworkJob::start();
}

bool MkColJob::finished()
{
    qCInfo(lcMkColJob) << "MKCOL of" << reply()->request().url() << "FINISHED WITH STATUS"
                       << replyStatusString();

    emit finished(reply()->error());
    return true;
}

/*********************************************************************************************/
// supposed to read <D:collection> when pointing to <D:resourcetype><D:collection></D:resourcetype>..
static QString readContentsAsString(QXmlStreamReader &reader)
{
    QString result;
    int level = 0;
    do {
        QXmlStreamReader::TokenType type = reader.readNext();
        if (type == QXmlStreamReader::StartElement) {
            level++;
            result += "<" + reader.name().toString() + ">";
        } else if (type == QXmlStreamReader::Characters) {
            result += reader.text();
        } else if (type == QXmlStreamReader::EndElement) {
            level--;
            if (level < 0) {
                break;
            }
            result += "</" + reader.name().toString() + ">";
        }

    } while (!reader.atEnd());
    return result;
}


LsColXMLParser::LsColXMLParser() = default;

bool LsColXMLParser::parse(const QByteArray &xml, QHash<QString, ExtraFolderInfo> *fileInfo, const QString &expectedPath)
{
    // Parse DAV response
    QXmlStreamReader reader(xml);
    reader.addExtraNamespaceDeclaration(QXmlStreamNamespaceDeclaration("d", "DAV:"));

    QStringList folders;
    QString currentHref;
    QMap<QString, QString> currentTmpProperties;
    QMap<QString, QString> currentHttp200Properties;
    bool currentPropsHaveHttp200 = false;
    bool insidePropstat = false;
    bool insideProp = false;
    bool insideMultiStatus = false;

    while (!reader.atEnd()) {
        QXmlStreamReader::TokenType type = reader.readNext();
        QString name = reader.name().toString();
        // Start elements with DAV:
        if (type == QXmlStreamReader::StartElement && reader.namespaceUri() == QLatin1String("DAV:")) {
            if (name == QLatin1String("href")) {
                // We don't use URL encoding in our request URL (which is the expected path) (QNAM will do it for us)
                // but the result will have URL encoding..
                QString hrefString = QUrl::fromLocalFile(QUrl::fromPercentEncoding(reader.readElementText().toUtf8()))
                        .adjusted(QUrl::NormalizePathSegments)
                        .path();
                if (!hrefString.startsWith(expectedPath)) {
                    qCWarning(lcLsColJob) << "Invalid href" << hrefString << "expected starting with" << expectedPath;
                    return false;
                }
                currentHref = hrefString;
            } else if (name == QLatin1String("response")) {
            } else if (name == QLatin1String("propstat")) {
                insidePropstat = true;
            } else if (name == QLatin1String("status") && insidePropstat) {
                QString httpStatus = reader.readElementText();
                if (httpStatus.startsWith("HTTP/1.1 200")) {
                    currentPropsHaveHttp200 = true;
                } else {
                    currentPropsHaveHttp200 = false;
                }
            } else if (name == QLatin1String("prop")) {
                insideProp = true;
                continue;
            } else if (name == QLatin1String("multistatus")) {
                insideMultiStatus = true;
                continue;
            }
        }

        if (type == QXmlStreamReader::StartElement && insidePropstat && insideProp) {
            // All those elements are properties
            QString propertyContent = readContentsAsString(reader);
            if (name == QLatin1String("resourcetype") && propertyContent.contains("collection")) {
                folders.append(currentHref);
            } else if (name == QLatin1String("size")) {
                bool ok = false;
                auto s = propertyContent.toLongLong(&ok);
                if (ok && fileInfo) {
                    (*fileInfo)[currentHref].size = s;
                }
            } else if (name == QLatin1String("fileid")) {
                (*fileInfo)[currentHref].fileId = propertyContent.toUtf8();
            }
            currentTmpProperties.insert(reader.name().toString(), propertyContent);
        }

        // End elements with DAV:
        if (type == QXmlStreamReader::EndElement) {
            if (reader.namespaceUri() == QLatin1String("DAV:")) {
                if (reader.name() == "response") {
                    if (currentHref.endsWith('/')) {
                        currentHref.chop(1);
                    }
                    emit directoryListingIterated(currentHref, currentHttp200Properties);
                    currentHref.clear();
                    currentHttp200Properties.clear();
                } else if (reader.name() == "propstat") {
                    insidePropstat = false;
                    if (currentPropsHaveHttp200) {
                        currentHttp200Properties = QMap<QString, QString>(currentTmpProperties);
                    }
                    currentTmpProperties.clear();
                    currentPropsHaveHttp200 = false;
                } else if (reader.name() == "prop") {
                    insideProp = false;
                }
            }
        }
    }

    if (reader.hasError()) {
        // XML Parser error? Whatever had been emitted before will come as directoryListingIterated
        qCWarning(lcLsColJob) << "ERROR" << reader.errorString() << xml;
        return false;
    } else if (!insideMultiStatus) {
        qCWarning(lcLsColJob) << "ERROR no WebDAV response?" << xml;
        return false;
    } else {
        emit directoryListingSubfolders(folders);
        emit finishedWithoutError();
    }
    return true;
}

/*********************************************************************************************/

LsColJob::LsColJob(AccountPtr account, const QString &path, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
{
}

LsColJob::LsColJob(AccountPtr account, const QUrl &url, QObject *parent)
    : AbstractNetworkJob(account, QString(), parent)
    , _url(url)
{
}

void LsColJob::setProperties(QList<QByteArray> properties)
{
    _properties = properties;
}

QList<QByteArray> LsColJob::properties() const
{
    return _properties;
}

void LsColJob::start()
{
    QList<QByteArray> properties = _properties;

    if (properties.isEmpty()) {
        qCWarning(lcLsColJob) << "Propfind with no properties!";
    }
    QByteArray propStr;
    foreach (const QByteArray &prop, properties) {
        if (prop.contains(':')) {
            int colIdx = prop.lastIndexOf(":");
            auto ns = prop.left(colIdx);
            if (ns == "http://owncloud.org/ns") {
                propStr += "    <oc:" + prop.mid(colIdx + 1) + " />\n";
            } else {
                propStr += "    <" + prop.mid(colIdx + 1) + " xmlns=\"" + ns + "\" />\n";
            }
        } else {
            propStr += "    <d:" + prop + " />\n";
        }
    }

    QNetworkRequest req;
    req.setRawHeader("Depth", "1");
    QByteArray xml("<?xml version=\"1.0\" ?>\n"
                   "<d:propfind xmlns:d=\"DAV:\" xmlns:oc=\"http://owncloud.org/ns\">\n"
                   "  <d:prop>\n"
        + propStr + "  </d:prop>\n"
                    "</d:propfind>\n");
    auto *buf = new QBuffer(this);
    buf->setData(xml);
    buf->open(QIODevice::ReadOnly);
    if (_url.isValid()) {
        sendRequest("PROPFIND", _url, req, buf);
    } else {
        sendRequest("PROPFIND", makeDavUrl(path()), req, buf);
    }
    AbstractNetworkJob::start();
}

// TODO: Instead of doing all in this slot, we should iteratively parse in readyRead(). This
// would allow us to be more asynchronous in processing while data is coming from the network,
// not all in one big blob at the end.
bool LsColJob::finished()
{
    qCInfo(lcLsColJob) << "LSCOL of" << reply()->request().url() << "FINISHED WITH STATUS"
                       << replyStatusString();

    QString contentType = reply()->header(QNetworkRequest::ContentTypeHeader).toString();
    int httpCode = reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (httpCode == 207 && contentType.contains("application/xml; charset=utf-8")) {
        LsColXMLParser parser;
        connect(&parser, &LsColXMLParser::directoryListingSubfolders,
            this, &LsColJob::directoryListingSubfolders);
        connect(&parser, &LsColXMLParser::directoryListingIterated,
            this, &LsColJob::directoryListingIterated);
        connect(&parser, &LsColXMLParser::finishedWithError,
            this, &LsColJob::finishedWithError);
        connect(&parser, &LsColXMLParser::finishedWithoutError,
            this, &LsColJob::finishedWithoutError);

        QString expectedPath = reply()->request().url().path(); // something like "/owncloud/remote.php/webdav/folder"
        if (!parser.parse(reply()->readAll(), &_folderInfos, expectedPath)) {
            // XML parse error
            emit finishedWithError(reply());
        }
    } else {
        // wrong content type, wrong HTTP code or any other network error
        emit finishedWithError(reply());
    }

    return true;
}

/*********************************************************************************************/

namespace {
    const char statusphpC[] = "status.php";
    const char nextcloudDirC[] = "nextcloud/";
}

CheckServerJob::CheckServerJob(AccountPtr account, QObject *parent)
    : AbstractNetworkJob(account, QLatin1String(statusphpC), parent)
    , _subdirFallback(false)
    , _permanentRedirects(0)
{
    setIgnoreCredentialFailure(true);
    connect(this, &AbstractNetworkJob::redirected,
        this, &CheckServerJob::slotRedirected);
}

void CheckServerJob::start()
{
    _serverUrl = account()->url();
    sendRequest("GET", Utility::concatUrlPath(_serverUrl, path()));
    connect(reply(), &QNetworkReply::metaDataChanged, this, &CheckServerJob::metaDataChangedSlot);
    connect(reply(), &QNetworkReply::encrypted, this, &CheckServerJob::encryptedSlot);
    AbstractNetworkJob::start();
}

void CheckServerJob::onTimedOut()
{
    qCWarning(lcCheckServerJob) << "TIMEOUT";
    if (reply() && reply()->isRunning()) {
        emit timeout(reply()->url());
    } else if (!reply()) {
        qCWarning(lcCheckServerJob) << "Timeout even there was no reply?";
    }
    deleteLater();
}

QString CheckServerJob::version(const QJsonObject &info)
{
    return info.value(QLatin1String("version")).toString();
}

QString CheckServerJob::versionString(const QJsonObject &info)
{
    return info.value(QLatin1String("versionstring")).toString();
}

bool CheckServerJob::installed(const QJsonObject &info)
{
    return info.value(QLatin1String("installed")).toBool();
}

static void mergeSslConfigurationForSslButton(const QSslConfiguration &config, AccountPtr account)
{
    if (config.peerCertificateChain().length() > 0) {
        account->_peerCertificateChain = config.peerCertificateChain();
    }
    if (!config.sessionCipher().isNull()) {
        account->_sessionCipher = config.sessionCipher();
    }
    if (config.sessionTicket().length() > 0) {
        account->_sessionTicket = config.sessionTicket();
    }
}

void CheckServerJob::encryptedSlot()
{
    mergeSslConfigurationForSslButton(reply()->sslConfiguration(), account());
}

void CheckServerJob::slotRedirected(QNetworkReply *reply, const QUrl &targetUrl, int redirectCount)
{
    QByteArray slashStatusPhp("/");
    slashStatusPhp.append(statusphpC);

    int httpCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString path = targetUrl.path();
    if ((httpCode == 301 || httpCode == 308) // permanent redirection
        && redirectCount == _permanentRedirects // don't apply permanent redirects after a temporary one
        && path.endsWith(slashStatusPhp)) {
        _serverUrl = targetUrl;
        _serverUrl.setPath(path.left(path.size() - slashStatusPhp.size()));
        qCInfo(lcCheckServerJob) << "status.php was permanently redirected to"
                                 << targetUrl << "new server url is" << _serverUrl;
        ++_permanentRedirects;
    }
}

void CheckServerJob::metaDataChangedSlot()
{
    account()->setSslConfiguration(reply()->sslConfiguration());
    mergeSslConfigurationForSslButton(reply()->sslConfiguration(), account());
}


bool CheckServerJob::finished()
{
    if (reply()->request().url().scheme() == QLatin1String("https")
        && reply()->sslConfiguration().sessionTicket().isEmpty()
        && reply()->error() == QNetworkReply::NoError) {
        qCWarning(lcCheckServerJob) << "No SSL session identifier / session ticket is used, this might impact sync performance negatively.";
    }

    mergeSslConfigurationForSslButton(reply()->sslConfiguration(), account());

    // The server installs to /owncloud. Let's try that if the file wasn't found
    // at the original location
    if ((reply()->error() == QNetworkReply::ContentNotFoundError) && (!_subdirFallback)) {
        _subdirFallback = true;
        setPath(QLatin1String(nextcloudDirC) + QLatin1String(statusphpC));
        start();
        qCInfo(lcCheckServerJob) << "Retrying with" << reply()->url();
        return false;
    }

    QByteArray body = reply()->peek(4 * 1024);
    int httpStatus = reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (body.isEmpty() || httpStatus != 200) {
        qCWarning(lcCheckServerJob) << "error: status.php replied " << httpStatus << body;
        emit instanceNotFound(reply());
    } else {
        QJsonParseError error;
        auto status = QJsonDocument::fromJson(body, &error);
        // empty or invalid response
        if (error.error != QJsonParseError::NoError || status.isNull()) {
            qCWarning(lcCheckServerJob) << "status.php from server is not valid JSON!" << body << reply()->request().url() << error.errorString();
        }

        qCInfo(lcCheckServerJob) << "status.php returns: " << status << " " << reply()->error() << " Reply: " << reply();
        if (status.object().contains("installed")) {
            emit instanceFound(_serverUrl, status.object());
        } else {
            qCWarning(lcCheckServerJob) << "No proper answer on " << reply()->url();
            emit instanceNotFound(reply());
        }
    }
    return true;
}

/*********************************************************************************************/

PropfindJob::PropfindJob(AccountPtr account, const QString &path, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
{
}

void PropfindJob::start()
{
    QList<QByteArray> properties = _properties;

    if (properties.isEmpty()) {
        qCWarning(lcLsColJob) << "Propfind with no properties!";
    }
    QNetworkRequest req;
    // Always have a higher priority than the propagator because we use this from the UI
    // and really want this to be done first (no matter what internal scheduling QNAM uses).
    // Also possibly useful for avoiding false timeouts.
    req.setPriority(QNetworkRequest::HighPriority);
    req.setRawHeader("Depth", "0");
    QByteArray propStr;
    foreach (const QByteArray &prop, properties) {
        if (prop.contains(':')) {
            int colIdx = prop.lastIndexOf(":");
            propStr += "    <" + prop.mid(colIdx + 1) + " xmlns=\"" + prop.left(colIdx) + "\" />\n";
        } else {
            propStr += "    <d:" + prop + " />\n";
        }
    }
    QByteArray xml = "<?xml version=\"1.0\" ?>\n"
                     "<d:propfind xmlns:d=\"DAV:\">\n"
                     "  <d:prop>\n"
        + propStr + "  </d:prop>\n"
                    "</d:propfind>\n";

    auto *buf = new QBuffer(this);
    buf->setData(xml);
    buf->open(QIODevice::ReadOnly);
    sendRequest("PROPFIND", makeDavUrl(path()), req, buf);

    AbstractNetworkJob::start();
}

void PropfindJob::setProperties(QList<QByteArray> properties)
{
    _properties = properties;
}

QList<QByteArray> PropfindJob::properties() const
{
    return _properties;
}

bool PropfindJob::finished()
{
    qCInfo(lcPropfindJob) << "PROPFIND of" << reply()->request().url() << "FINISHED WITH STATUS"
                          << replyStatusString();

    int http_result_code = reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (http_result_code == 207) {
        // Parse DAV response
        QXmlStreamReader reader(reply());
        reader.addExtraNamespaceDeclaration(QXmlStreamNamespaceDeclaration("d", "DAV:"));

        QVariantMap items;
        // introduced to nesting is ignored
        QStack<QString> curElement;

        while (!reader.atEnd()) {
            QXmlStreamReader::TokenType type = reader.readNext();
            if (type == QXmlStreamReader::StartElement) {
                if (!curElement.isEmpty() && curElement.top() == QLatin1String("prop")) {
                    items.insert(reader.name().toString(), reader.readElementText(QXmlStreamReader::SkipChildElements));
                } else {
                    curElement.push(reader.name().toString());
                }
            }
            if (type == QXmlStreamReader::EndElement) {
                if (curElement.top() == reader.name()) {
                    curElement.pop();
                }
            }
        }
        if (reader.hasError()) {
            qCWarning(lcPropfindJob) << "XML parser error: " << reader.errorString();
            emit finishedWithError(reply());
        } else {
            emit result(items);
        }
    } else {
        qCWarning(lcPropfindJob) << "*not* successful, http result code is" << http_result_code
                                 << (http_result_code == 302 ? reply()->header(QNetworkRequest::LocationHeader).toString() : QLatin1String(""));
        emit finishedWithError(reply());
    }
    return true;
}

/*********************************************************************************************/

#ifndef TOKEN_AUTH_ONLY
AvatarJob::AvatarJob(AccountPtr account, const QString &userId, int size, QObject *parent)
    : AbstractNetworkJob(account, QString(), parent)
{
    if (account->serverVersionInt() >= Account::makeServerVersion(10, 0, 0)) {
        _avatarUrl = Utility::concatUrlPath(account->url(), QString("remote.php/dav/avatars/%1/%2.png").arg(userId, QString::number(size)));
    } else {
        _avatarUrl = Utility::concatUrlPath(account->url(), QString("index.php/avatar/%1/%2").arg(userId, QString::number(size)));
    }
}

void AvatarJob::start()
{
    QNetworkRequest req;
    sendRequest("GET", _avatarUrl, req);
    AbstractNetworkJob::start();
}

QImage AvatarJob::makeCircularAvatar(const QImage &baseAvatar)
{
    if (baseAvatar.isNull()) {
        return {};
    }

    int dim = baseAvatar.width();

    QImage avatar(dim, dim, QImage::Format_ARGB32);
    avatar.fill(Qt::transparent);

    QPainter painter(&avatar);
    painter.setRenderHint(QPainter::Antialiasing);

    QPainterPath path;
    path.addEllipse(0, 0, dim, dim);
    painter.setClipPath(path);

    painter.drawImage(0, 0, baseAvatar);
    painter.end();

    return avatar;
}

bool AvatarJob::finished()
{
    int http_result_code = reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    QImage avImage;

    if (http_result_code == 200) {
        QByteArray pngData = reply()->readAll();
        if (pngData.size()) {
            if (avImage.loadFromData(pngData)) {
                qCDebug(lcAvatarJob) << "Retrieved Avatar pixmap!";
            }
        }
    }
    emit(avatarPixmap(avImage));
    return true;
}
#endif

/*********************************************************************************************/

ProppatchJob::ProppatchJob(AccountPtr account, const QString &path, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
{
}

void ProppatchJob::start()
{
    if (_properties.isEmpty()) {
        qCWarning(lcProppatchJob) << "Proppatch with no properties!";
    }
    QNetworkRequest req;

    QByteArray propStr;
    QMapIterator<QByteArray, QByteArray> it(_properties);
    while (it.hasNext()) {
        it.next();
        QByteArray keyName = it.key();
        QByteArray keyNs;
        if (keyName.contains(':')) {
            int colIdx = keyName.lastIndexOf(":");
            keyNs = keyName.left(colIdx);
            keyName = keyName.mid(colIdx + 1);
        }

        propStr += "    <" + keyName;
        if (!keyNs.isEmpty()) {
            propStr += " xmlns=\"" + keyNs + "\" ";
        }
        propStr += ">";
        propStr += it.value();
        propStr += "</" + keyName + ">\n";
    }
    QByteArray xml = "<?xml version=\"1.0\" ?>\n"
                     "<d:propertyupdate xmlns:d=\"DAV:\">\n"
                     "  <d:set><d:prop>\n"
        + propStr + "  </d:prop></d:set>\n"
                    "</d:propertyupdate>\n";

    auto *buf = new QBuffer(this);
    buf->setData(xml);
    buf->open(QIODevice::ReadOnly);
    sendRequest("PROPPATCH", makeDavUrl(path()), req, buf);
    AbstractNetworkJob::start();
}

void ProppatchJob::setProperties(QMap<QByteArray, QByteArray> properties)
{
    _properties = properties;
}

QMap<QByteArray, QByteArray> ProppatchJob::properties() const
{
    return _properties;
}

bool ProppatchJob::finished()
{
    qCInfo(lcProppatchJob) << "PROPPATCH of" << reply()->request().url() << "FINISHED WITH STATUS"
                           << replyStatusString();

    int http_result_code = reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    if (http_result_code == 207) {
        emit success();
    } else {
        qCWarning(lcProppatchJob) << "*not* successful, http result code is" << http_result_code
                                  << (http_result_code == 302 ? reply()->header(QNetworkRequest::LocationHeader).toString() : QLatin1String(""));
        emit finishedWithError();
    }
    return true;
}

/*********************************************************************************************/

EntityExistsJob::EntityExistsJob(AccountPtr account, const QString &path, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
{
}

void EntityExistsJob::start()
{
    sendRequest("HEAD", makeAccountUrl(path()));
    AbstractNetworkJob::start();
}

bool EntityExistsJob::finished()
{
    emit exists(reply());
    return true;
}

/*********************************************************************************************/

JsonApiJob::JsonApiJob(const AccountPtr &account, const QString &path, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
{
}

void JsonApiJob::addQueryParams(const QUrlQuery &params)
{
    _additionalParams = params;
}

void JsonApiJob::addRawHeader(const QByteArray &headerName, const QByteArray &value)
{
   _request.setRawHeader(headerName, value);
}

void JsonApiJob::start()
{
    addRawHeader("OCS-APIREQUEST", "true");
    auto query = _additionalParams;
    query.addQueryItem(QLatin1String("format"), QLatin1String("json"));
    QUrl url = Utility::concatUrlPath(account()->url(), path(), query);
    sendRequest(_usePOST ? "POST" : "GET", url, _request);
    AbstractNetworkJob::start();
}

bool JsonApiJob::finished()
{
    qCInfo(lcJsonApiJob) << "JsonApiJob of" << reply()->request().url() << "FINISHED WITH STATUS"
                         << replyStatusString();

    int statusCode = 0;
    int httpStatusCode = reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply()->error() != QNetworkReply::NoError) {
        qCWarning(lcJsonApiJob) << "Network error: " << path() << errorString() << reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        statusCode = reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        emit jsonReceived(QJsonDocument(), statusCode);
        return true;
    }

    QString jsonStr = QString::fromUtf8(reply()->readAll());
    if (jsonStr.contains("<?xml version=\"1.0\"?>")) {
        QRegExp rex("<statuscode>(\\d+)</statuscode>");
        if (jsonStr.contains(rex)) {
            // this is a error message coming back from ocs.
            statusCode = rex.cap(1).toInt();
        }
    } else if(jsonStr.isEmpty() && httpStatusCode == notModifiedStatusCode){
        qCWarning(lcJsonApiJob) << "Nothing changed so nothing to retrieve - status code: " << httpStatusCode;
        statusCode = httpStatusCode;
    } else {
        QRegExp rex(R"("statuscode":(\d+),)");
        // example: "{"ocs":{"meta":{"status":"ok","statuscode":100,"message":null},"data":{"version":{"major":8,"minor":"... (504)
        if (jsonStr.contains(rex)) {
            statusCode = rex.cap(1).toInt();
        }
    }

    // save new ETag value
    if(reply()->rawHeaderList().contains("ETag"))
        emit etagResponseHeaderReceived(reply()->rawHeader("ETag"), statusCode);

    const auto desktopNotificationsAllowed = reply()->rawHeader(QByteArray("X-Nextcloud-User-Status"));
    if(!desktopNotificationsAllowed.isEmpty()) {
        emit allowDesktopNotificationsChanged(desktopNotificationsAllowed == "online");
    }

    QJsonParseError error;
    auto json = QJsonDocument::fromJson(jsonStr.toUtf8(), &error);
    // empty or invalid response and status code is != 304 because jsonStr is expected to be empty
    if ((error.error != QJsonParseError::NoError || json.isNull()) && httpStatusCode != notModifiedStatusCode) {
        qCWarning(lcJsonApiJob) << "invalid JSON!" << jsonStr << error.errorString();
        emit jsonReceived(json, statusCode);
        return true;
    }

    emit jsonReceived(json, statusCode);
    return true;
}


DetermineAuthTypeJob::DetermineAuthTypeJob(AccountPtr account, QObject *parent)
    : AbstractNetworkJob(account, QString(), parent)
{
    setAuthenticationJob(true);
    setIgnoreCredentialFailure(true);
}

void DetermineAuthTypeJob::start()
{
    qCInfo(lcDetermineAuthTypeJob) << "Determining auth type for" << _account->davUrl();

    QNetworkRequest req;
    // Prevent HttpCredentialsAccessManager from setting an Authorization header.
    req.setAttribute(HttpCredentials::DontAddCredentialsAttribute, true);
    // Don't reuse previous auth credentials
    req.setAttribute(QNetworkRequest::AuthenticationReuseAttribute, QNetworkRequest::Manual);
    sendRequest("PROPFIND", _account->davUrl(), req);
    AbstractNetworkJob::start();

    // Start three parallel requests

    // 1. determines whether it's a basic auth server
    /*auto get =*/ sendRequest("GET", _account->url(), req);

    // 2. checks the HTTP auth method.
    /*auto propfind =*/ sendRequest("PROPFIND", _account->davUrl(), req);

    // 3. Determines if the old flow has to be used (GS for now)
    auto oldFlowRequired = new JsonApiJob(_account, "/ocs/v2.php/cloud/capabilities", this);

//    get->setTimeout(30 * 1000);
//    propfind->setTimeout(30 * 1000);
//    oldFlowRequired->setTimeout(30 * 1000);
//    get->setIgnoreCredentialFailure(true);
//    propfind->setIgnoreCredentialFailure(true);
//    oldFlowRequired->setIgnoreCredentialFailure(true);

//    connect(get, &SimpleNetworkJob::finishedSignal, this, [this, get]() {
//        if (get->reply()->error() == QNetworkReply::AuthenticationRequiredError) {
//            _resultGet = Basic;
//        } else {
//            _resultGet = LoginFlowV2;
//        }
//        _getDone = true;
//        checkAllDone();
//    });
//    connect(propfind, &SimpleNetworkJob::finishedSignal, this, [this](QNetworkReply *reply) {
//        auto authChallenge = reply->rawHeader("WWW-Authenticate").toLower();
//        if (authChallenge.contains("bearer ")) {
//            _resultPropfind = OAuth;
//        } else {
//            if (authChallenge.isEmpty()) {
//                qCWarning(lcDetermineAuthTypeJob) << "Did not receive WWW-Authenticate reply to auth-test PROPFIND";
//            } else {
//                qCWarning(lcDetermineAuthTypeJob) << "Unknown WWW-Authenticate reply to auth-test PROPFIND:" << authChallenge;
//            }
//            _resultPropfind = Basic;
//        }
//        _propfindDone = true;
//        checkAllDone();
//    });
    connect(oldFlowRequired, &JsonApiJob::jsonReceived, this, [this](const QJsonDocument &json, int statusCode) {
        if (statusCode == 200) {
            _resultOldFlow = LoginFlowV2;

            auto data = json.object().value("ocs").toObject().value("data").toObject().value("capabilities").toObject();
            auto gs = data.value("globalscale");
            if (gs != QJsonValue::Undefined) {
                auto flow = gs.toObject().value("desktoplogin");
                if (flow != QJsonValue::Undefined) {
                    if (flow.toInt() == 1) {
                        _resultOldFlow = WebViewFlow;
                    }
                }
            }
        } else {
            _resultOldFlow = Basic;
        }
        _oldFlowDone = true;
        checkAllDone();
    });

    oldFlowRequired->start();
}

void DetermineAuthTypeJob::checkAllDone()
{
    // Do not conitunue until eve
    if (!_getDone || !_propfindDone || !_oldFlowDone) {
        return;
    }

    Q_ASSERT(_resultGet != NoAuthType);
    Q_ASSERT(_resultPropfind != NoAuthType);
    Q_ASSERT(_resultOldFlow != NoAuthType);

    auto result = _resultPropfind;

    // WebViewFlow > OAuth > Basic
    if (_account->serverVersionInt() >= Account::makeServerVersion(12, 0, 0)) {
        result = WebViewFlow;
    }

    // LoginFlowV2 > WebViewFlow > OAuth > Basic
    if (_account->serverVersionInt() >= Account::makeServerVersion(16, 0, 0)) {
        result = LoginFlowV2;
    }

    // If we determined that we need the webview flow (GS for example) then we switch to that
    if (_resultOldFlow == WebViewFlow) {
        result = WebViewFlow;
    }

    // If we determined that a simple get gave us an authentication required error
    // then the server enforces basic auth and we got no choice but to use this
    if (_resultGet == Basic) {
        result = Basic;
    }

    qCInfo(lcDetermineAuthTypeJob) << "Auth type for" << _account->davUrl() << "is" << result;
    emit authType(result);
    deleteLater();
}

bool DetermineAuthTypeJob::finished()
{
    auto authChallenge = reply()->rawHeader("WWW-Authenticate").toLower();
    auto result = AuthType::Basic;
    if (authChallenge.contains("bearer ")) {
        result = AuthType::OAuth;
    } else if (authChallenge.isEmpty()) {
        qCWarning(lcDetermineAuthTypeJob) << "Did not receive WWW-Authenticate reply to auth-test PROPFIND";
    }
    qCInfo(lcDetermineAuthTypeJob) << "Auth type for" << _account->davUrl() << "is" << result;
    emit this->authType(result);
    return true;
}

SimpleNetworkJob::SimpleNetworkJob(AccountPtr account, QObject *parent)
    : AbstractNetworkJob(account, QString(), parent)
{
}

void SimpleNetworkJob::start()
{
    sendRequest(_simpleVerb, _simpleUrl, _simpleRequest, _simpleBody);
    AbstractNetworkJob::start();
}

void SimpleNetworkJob::prepareRequest(const QByteArray &verb, const QUrl &url,
    QNetworkRequest req, QIODevice *requestBody)
{
    _simpleVerb = verb;
    _simpleUrl = url;
    _simpleRequest = req;
    _simpleBody = requestBody;
}

bool SimpleNetworkJob::finished()
{
    emit finishedSignal(reply());
    return true;
}


DeleteApiJob::DeleteApiJob(AccountPtr account, const QString &path, QObject *parent)
    : AbstractNetworkJob(account, path, parent)
{

}

void DeleteApiJob::start()
{
    QNetworkRequest req;
    req.setRawHeader("OCS-APIREQUEST", "true");
    QUrl url = Utility::concatUrlPath(account()->url(), path());
    sendRequest("DELETE", url, req);
    AbstractNetworkJob::start();
}

bool DeleteApiJob::finished()
{
    qCInfo(lcJsonApiJob) << "JsonApiJob of" << reply()->request().url() << "FINISHED WITH STATUS"
                         << reply()->error()
                         << (reply()->error() == QNetworkReply::NoError ? QLatin1String("") : errorString());

    int httpStatus = reply()->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();


    if (reply()->error() != QNetworkReply::NoError) {
        qCWarning(lcJsonApiJob) << "Network error: " << path() << errorString() << httpStatus;
        emit result(httpStatus);
        return true;
    }

    const auto replyData = QString::fromUtf8(reply()->readAll());
    qCInfo(lcJsonApiJob()) << "TMX Delete Job" << replyData;
    emit result(httpStatus);
    return true;
}

void fetchPrivateLinkUrl(AccountPtr account, const QString &remotePath,
    const QByteArray &numericFileId, QObject *target,
    std::function<void(const QString &url)> targetFun)
{
    QString oldUrl;
    if (!numericFileId.isEmpty())
        oldUrl = account->deprecatedPrivateLinkUrl(numericFileId).toString(QUrl::FullyEncoded);

    // Retrieve the new link by PROPFIND
    auto *job = new PropfindJob(account, remotePath, target);
    job->setProperties(
        QList<QByteArray>()
        << "http://owncloud.org/ns:fileid" // numeric file id for fallback private link generation
        << "http://owncloud.org/ns:privatelink");
    job->setTimeout(10 * 1000);
    QObject::connect(job, &PropfindJob::result, target, [=](const QVariantMap &result) {
        auto privateLinkUrl = result["privatelink"].toString();
        auto numericFileId = result["fileid"].toByteArray();
        if (!privateLinkUrl.isEmpty()) {
            targetFun(privateLinkUrl);
        } else if (!numericFileId.isEmpty()) {
            targetFun(account->deprecatedPrivateLinkUrl(numericFileId).toString(QUrl::FullyEncoded));
        } else {
            targetFun(oldUrl);
        }
    });
    QObject::connect(job, &PropfindJob::finishedWithError, target, [=](QNetworkReply *) {
        targetFun(oldUrl);
    });
    job->start();
}

} // namespace OCC
