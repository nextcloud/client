/*
 * Copyright (C) by Olivier Goffart <ogoffart@owncloud.com>
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 * Copyright (C) by Daniel Heule <daniel.heule@gmail.com>
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

#include <iostream>
#include <random>
#include <qcoreapplication.h>
#include <QStringList>
#include <QUrl>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkProxy>
#include <qdebug.h>

#include "account.h"
#include "configfile.h" // ONLY ACCESS THE STATIC FUNCTIONS!
#ifdef TOKEN_AUTH_ONLY
# include "creds/tokencredentials.h"
#else
# include "creds/httpcredentials.h"
#endif
#include "simplesslerrorhandler.h"
#include "syncengine.h"
#include "common/syncjournaldb.h"
#include "config.h"
#include "csync_exclude.h"


#include "cmd.h"

#include "theme.h"
#include "netrcparser.h"
#include "libsync/logger.h"

#include "config.h"

#ifdef Q_OS_WIN32
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

using namespace OCC;


static void nullMessageHandler(QtMsgType, const QMessageLogContext &, const QString &)
{
}

struct CmdOptions
{
    QString source_dir;
    QString target_url;
    QString config_directory;
    QString user;
    QString password;
    QString proxy;
    bool silent;
    bool trustSSL;
    bool useNetrc;
    bool interactive;
    bool ignoreHiddenFiles;
    bool nonShib;
    QString exclude;
    QString unsyncedfolders;
    QString davPath;
    int restartTimes;
    int downlimit;
    int uplimit;
};

// we can't use csync_set_userdata because the SyncEngine sets it already.
// So we have to use a global variable
CmdOptions *opts = nullptr;

class EchoDisabler
{
public:
    EchoDisabler()
    {
#ifdef Q_OS_WIN
        hStdin = GetStdHandle(STD_INPUT_HANDLE);
        GetConsoleMode(hStdin, &mode);
        SetConsoleMode(hStdin, mode & (~ENABLE_ECHO_INPUT));
#else
        tcgetattr(STDIN_FILENO, &tios);
        termios tios_new = tios;
        tios_new.c_lflag &= ~ECHO;
        tcsetattr(STDIN_FILENO, TCSANOW, &tios_new);
#endif
    }

    ~EchoDisabler()
    {
#ifdef Q_OS_WIN
        SetConsoleMode(hStdin, mode);
#else
        tcsetattr(STDIN_FILENO, TCSANOW, &tios);
#endif
    }

private:
#ifdef Q_OS_WIN
    DWORD mode = 0;
    HANDLE hStdin;
#else
    termios tios;
#endif
};

QString queryPassword(const QString &user)
{
    EchoDisabler disabler;
    std::cout << "Password for user " << qPrintable(user) << ": ";
    std::string s;
    std::getline(std::cin, s);
    return QString::fromStdString(s);
}

#ifndef TOKEN_AUTH_ONLY
class HttpCredentialsText : public HttpCredentials
{
public:
    HttpCredentialsText(const QString &user, const QString &password)
        : HttpCredentials(user, password)
        , // FIXME: not working with client certs yet (qknight)
        _sslTrusted(false)
    {
    }

    void askFromUser() Q_DECL_OVERRIDE
    {
        _password = ::queryPassword(user());
        _ready = true;
        persist();
        emit asked();
    }

    void setSSLTrusted(bool isTrusted)
    {
        _sslTrusted = isTrusted;
    }

    bool sslIsTrusted() Q_DECL_OVERRIDE
    {
        return _sslTrusted;
    }

private:
    bool _sslTrusted;
};
#endif /* TOKEN_AUTH_ONLY */

void help()
{
    const char *binaryName = APPLICATION_EXECUTABLE "cmd";

    std::cout << binaryName << " - command line " APPLICATION_NAME " client tool"; std::cout.flush();
    std::cout << ""; std::cout.flush();
    std::cout << "Usage: " << binaryName << " [OPTION] <source_dir> <server_url>"; std::cout.flush();
    std::cout << ""; std::cout.flush();
    std::cout << "A proxy can either be set manually using --httpproxy."; std::cout.flush();
    std::cout << "Otherwise, the setting from a configured sync client will be used."; std::cout.flush();
    std::cout << ""; std::cout.flush();
    std::cout << "Options:"; std::cout.flush();
    std::cout << "  --silent, -s           Don't be so verbose"; std::cout.flush();
    std::cout << "  --httpproxy [proxy]    Specify a http proxy to use."; std::cout.flush();
    std::cout << "                         Proxy is http://server:port"; std::cout.flush();
    std::cout << "  --trust                Trust the SSL certification."; std::cout.flush();
    std::cout << "  --exclude [file]       Exclude list file"; std::cout.flush();
    std::cout << "  --unsyncedfolders [file]    File containing the list of unsynced remote folders (selective sync)"; std::cout.flush();
    std::cout << "  --user, -u [name]      Use [name] as the login name"; std::cout.flush();
    std::cout << "  --password, -p [pass]  Use [pass] as password"; std::cout.flush();
    std::cout << "  -n                     Use netrc (5) for login"; std::cout.flush();
    std::cout << "  --non-interactive      Do not block execution with interaction"; std::cout.flush();
    std::cout << "  --nonshib              Use Non Shibboleth WebDAV authentication"; std::cout.flush();
    std::cout << "  --davpath [path]       Custom themed dav path, overrides --nonshib"; std::cout.flush();
    std::cout << "  --max-sync-retries [n] Retries maximum n times (default to 3)"; std::cout.flush();
    std::cout << "  --uplimit [n]          Limit the upload speed of files to n KB/s"; std::cout.flush();
    std::cout << "  --downlimit [n]        Limit the download speed of files to n KB/s"; std::cout.flush();
    std::cout << "  -h                     Sync hidden files, do not ignore them"; std::cout.flush();
    std::cout << "  --version, -v          Display version and exit"; std::cout.flush();
    std::cout << "  --logdebug             More verbose logging"; std::cout.flush();
    std::cout << ""; std::cout.flush();
    exit(0);
}

void showVersion()
{
    std::cout << qUtf8Printable(Theme::instance()->versionSwitchOutput());
    exit(0);
}

void parseOptions(const QStringList &app_args, CmdOptions *options)
{
    QStringList args(app_args);

    int argCount = args.count();

    if (argCount < 3) {
        if (argCount >= 2) {
            const QString option = args.at(1);
            if (option == "-v" || option == "--version") {
                showVersion();
            }
        }
        help();
    }

    options->target_url = args.takeLast();

    options->source_dir = args.takeLast();
    if (!options->source_dir.endsWith('/')) {
        options->source_dir.append('/');
    }
    QFileInfo fi(options->source_dir);
    if (!fi.exists()) {
        std::cerr << "Source dir '" << qPrintable(options->source_dir) << "' does not exist."; std::cerr.flush();
        exit(1);
    }
    options->source_dir = fi.absoluteFilePath();

    QStringListIterator it(args);
    // skip file name;
    if (it.hasNext())
        it.next();

    while (it.hasNext()) {
        const QString option = it.next();

        if (option == "--httpproxy" && !it.peekNext().startsWith("-")) {
            options->proxy = it.next();
        } else if (option == "-s" || option == "--silent") {
            options->silent = true;
        } else if (option == "--trust") {
            options->trustSSL = true;
        } else if (option == "-n") {
            options->useNetrc = true;
        } else if (option == "-h") {
            options->ignoreHiddenFiles = false;
        } else if (option == "--non-interactive") {
            options->interactive = false;
        } else if ((option == "-u" || option == "--user") && !it.peekNext().startsWith("-")) {
            options->user = it.next();
        } else if ((option == "-p" || option == "--password") && !it.peekNext().startsWith("-")) {
            options->password = it.next();
        } else if (option == "--exclude" && !it.peekNext().startsWith("-")) {
            options->exclude = it.next();
        } else if (option == "--unsyncedfolders" && !it.peekNext().startsWith("-")) {
            options->unsyncedfolders = it.next();
        } else if (option == "--nonshib") {
            options->nonShib = true;
        } else if (option == "--davpath" && !it.peekNext().startsWith("-")) {
            options->davPath = it.next();
        } else if (option == "--max-sync-retries" && !it.peekNext().startsWith("-")) {
            options->restartTimes = it.next().toInt();
        } else if (option == "--uplimit" && !it.peekNext().startsWith("-")) {
            options->uplimit = it.next().toInt() * 1000;
        } else if (option == "--downlimit" && !it.peekNext().startsWith("-")) {
            options->downlimit = it.next().toInt() * 1000;
        } else if (option == "--logdebug") {
            Logger::instance()->setLogFile("-");
            Logger::instance()->setLogDebug(true);
        } else {
            help();
        }
    }

    if (options->target_url.isEmpty() || options->source_dir.isEmpty()) {
        help();
    }
}

/* If the selective sync list is different from before, we need to disable the read from db
  (The normal client does it in SelectiveSyncDialog::accept*)
 */
void selectiveSyncFixup(OCC::SyncJournalDb *journal, const QStringList &newList)
{
    SqlDatabase db;
    if (!db.openOrCreateReadWrite(journal->databaseFilePath())) {
        return;
    }

    bool ok = false;

    auto oldBlackListSet = journal->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, &ok).toSet();
    if (ok) {
        auto blackListSet = newList.toSet();
        const auto changes = (oldBlackListSet - blackListSet) + (blackListSet - oldBlackListSet);
        for (const auto &it : changes) {
            journal->schedulePathForRemoteDiscovery(it);
        }

        journal->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, newList);
    }
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

#ifdef Q_OS_WIN
    // Ensure OpenSSL config file is only loaded from app directory
    QString opensslConf = QCoreApplication::applicationDirPath() + QString("/openssl.cnf");
    qputenv("OPENSSL_CONF", opensslConf.toLocal8Bit());
#endif

    qsrand(std::random_device()());

    CmdOptions options;
    options.silent = false;
    options.trustSSL = false;
    options.useNetrc = false;
    options.interactive = true;
    options.ignoreHiddenFiles = false; // Default is to sync hidden files
    options.nonShib = false;
    options.restartTimes = 3;
    options.uplimit = 0;
    options.downlimit = 0;

    parseOptions(app.arguments(), &options);

    if (options.silent) {
        qInstallMessageHandler(nullMessageHandler);
    } else {
        qSetMessagePattern("%{time MM-dd hh:mm:ss:zzz} [ %{type} %{category} ]%{if-debug}\t[ %{function} ]%{endif}:\t%{message}");
    }

    AccountPtr account = Account::create();

    if (!account) {
        qFatal("Could not initialize account!");
        return EXIT_FAILURE;
    }
    // check if the webDAV path was added to the url and append if not.
    if (!options.target_url.endsWith("/")) {
        options.target_url.append("/");
    }

    if (options.nonShib) {
        account->setNonShib(true);
    }

    if (!options.davPath.isEmpty()) {
        account->setDavPath(options.davPath);
    }

    if (!options.target_url.contains(account->davPath())) {
        options.target_url.append(account->davPath());
    }

    QUrl url = QUrl::fromUserInput(options.target_url);

    // Order of retrieval attempt (later attempts override earlier ones):
    // 1. From URL
    // 2. From options
    // 3. From netrc (if enabled)
    // 4. From prompt (if interactive)

    QString user = url.userName();
    QString password = url.password();

    if (!options.user.isEmpty()) {
        user = options.user;
    }

    if (!options.password.isEmpty()) {
        password = options.password;
    }

    if (options.useNetrc) {
        NetrcParser parser;
        if (parser.parse()) {
            NetrcParser::LoginPair pair = parser.find(url.host());
            user = pair.first;
            password = pair.second;
        }
    }

    if (options.interactive) {
        if (user.isEmpty()) {
            std::cout << "Please enter user name: ";
            std::string s;
            std::getline(std::cin, s);
            user = QString::fromStdString(s);
        }
        if (password.isEmpty()) {
            password = queryPassword(user);
        }
    }

    // take the unmodified url to pass to csync_create()
    QByteArray remUrl = options.target_url.toUtf8();

    // Find the folder and the original owncloud url
    QStringList splitted = url.path().split("/" + account->davPath());
    url.setPath(splitted.value(0));

    url.setScheme(url.scheme().replace("owncloud", "http"));

    QUrl credentialFreeUrl = url;
    credentialFreeUrl.setUserName(QString());
    credentialFreeUrl.setPassword(QString());

    // Remote folders typically start with a / and don't end with one
    QString folder = "/" + splitted.value(1);
    if (folder.endsWith("/") && folder != "/") {
        folder.chop(1);
    }

    if (!options.proxy.isNull()) {
        QString host;
        int port = 0;
        bool ok = false;

        QStringList pList = options.proxy.split(':');
        if (pList.count() == 3) {
            // http: //192.168.178.23 : 8080
            //  0            1            2
            host = pList.at(1);
            if (host.startsWith("//"))
                host.remove(0, 2);

            port = pList.at(2).toInt(&ok);

            QNetworkProxyFactory::setUseSystemConfiguration(false);
            QNetworkProxy::setApplicationProxy(QNetworkProxy(QNetworkProxy::HttpProxy, host, port));
        } else {
            qFatal("Could not read httpproxy. The proxy should have the format \"http://hostname:port\".");
        }
    }

    auto *sslErrorHandler = new SimpleSslErrorHandler;

#ifdef TOKEN_AUTH_ONLY
    auto *cred = new TokenCredentials(user, password, "");
    account->setCredentials(cred);
#else
    auto *cred = new HttpCredentialsText(user, password);
    account->setCredentials(cred);
    if (options.trustSSL) {
        cred->setSSLTrusted(true);
    }
#endif

    account->setUrl(url);
    account->setSslErrorHandler(sslErrorHandler);

    // Perform a call to get the capabilities.
    if (!options.nonShib) {
        // Do not do it if '--nonshib' was passed. This mean we should only connect to the 'nonshib'
        // dav endpoint. Since we do not get the capabilities, in that case, this has the additional
        // side effect that chunking-ng will be disabled. (because otherwise it would use the new
        // 'dav' endpoint instead of the nonshib one (which still use the old chunking)

        QEventLoop loop;
        auto *job = new JsonApiJob(account, QLatin1String("ocs/v1.php/cloud/capabilities"));
        QObject::connect(job, &JsonApiJob::jsonReceived, [&](const QJsonDocument &json) {
            auto caps = json.object().value("ocs").toObject().value("data").toObject().value("capabilities").toObject();
            qDebug() << "Server capabilities" << caps;
            account->setCapabilities(caps.toVariantMap());
            account->setServerVersion(caps["core"].toObject()["status"].toObject()["version"].toString());
            loop.quit();
        });
        job->start();
        loop.exec();

        if (job->reply()->error() != QNetworkReply::NoError){
            std::cout<<"Error connecting to server\n";
            return EXIT_FAILURE;
        }

        job = new JsonApiJob(account, QLatin1String("ocs/v1.php/cloud/user"));
        QObject::connect(job, &JsonApiJob::jsonReceived, [&](const QJsonDocument &json) {
            const QJsonObject data = json.object().value("ocs").toObject().value("data").toObject();
            account->setDavUser(data.value("id").toString());
            account->setDavDisplayName(data.value("display-name").toString());
            loop.quit();
        });
        job->start();
        loop.exec();
    }

    // much lower age than the default since this utility is usually made to be run right after a change in the tests
    SyncEngine::minimumFileAgeForUpload = std::chrono::milliseconds(0);

    int restartCount = 0;
restart_sync:

    opts = &options;

    QStringList selectiveSyncList;
    if (!options.unsyncedfolders.isEmpty()) {
        QFile f(options.unsyncedfolders);
        if (!f.open(QFile::ReadOnly)) {
            qCritical() << "Could not open file containing the list of unsynced folders: " << options.unsyncedfolders;
        } else {
            // filter out empty lines and comments
            selectiveSyncList = QString::fromUtf8(f.readAll()).split('\n').filter(QRegExp("\\S+")).filter(QRegExp("^[^#]"));

            for (int i = 0; i < selectiveSyncList.count(); ++i) {
                if (!selectiveSyncList.at(i).endsWith(QLatin1Char('/'))) {
                    selectiveSyncList[i].append(QLatin1Char('/'));
                }
            }
        }
    }

    Cmd cmd;
    QString dbPath = options.source_dir + SyncJournalDb::makeDbName(options.source_dir, credentialFreeUrl, folder, user);
    SyncJournalDb db(dbPath);

    if (!selectiveSyncList.empty()) {
        selectiveSyncFixup(&db, selectiveSyncList);
    }

    SyncEngine engine(account, options.source_dir, folder, &db);
    engine.setIgnoreHiddenFiles(options.ignoreHiddenFiles);
    engine.setNetworkLimits(options.uplimit, options.downlimit);
    QObject::connect(&engine, &SyncEngine::finished,
        [&app](bool result) { app.exit(result ? EXIT_SUCCESS : EXIT_FAILURE); });
    QObject::connect(&engine, &SyncEngine::transmissionProgress, &cmd, &Cmd::transmissionProgressSlot);
    QObject::connect(&engine, &SyncEngine::syncError,
        [](const QString &error) { qWarning() << "Sync error:" << error; });


    // Exclude lists

    bool hasUserExcludeFile = !options.exclude.isEmpty();
    QString systemExcludeFile = ConfigFile::excludeFileFromSystem();

    // Always try to load the user-provided exclude list if one is specified
    if (hasUserExcludeFile) {
        engine.excludedFiles().addExcludeFilePath(options.exclude);
    }
    // Load the system list if available, or if there's no user-provided list
    if (!hasUserExcludeFile || QFile::exists(systemExcludeFile)) {
        engine.excludedFiles().addExcludeFilePath(systemExcludeFile);
    }

    if (!engine.excludedFiles().reloadExcludeFiles()) {
        qFatal("Cannot load system exclude list or list supplied via --exclude");
        return EXIT_FAILURE;
    }


    // Have to be done async, else, an error before exec() does not terminate the event loop.
    QMetaObject::invokeMethod(&engine, "startSync", Qt::QueuedConnection);

    int resultCode = app.exec();

    if (engine.isAnotherSyncNeeded() != NoFollowUpSync) {
        if (restartCount < options.restartTimes) {
            restartCount++;
            qDebug() << "Restarting Sync, because another sync is needed" << restartCount;
            goto restart_sync;
        }
        qWarning() << "Another sync is needed, but not done because restart count is exceeded" << restartCount;
    }

    return resultCode;
}
