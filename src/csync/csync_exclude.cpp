/*
 * libcsync -- a library to sync a directory with another
 *
 * Copyright (c) 2008-2013 by Andreas Schneider <asn@cryptomilk.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include "config_csync.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "c_lib.h"
#include "c_private.h"

#include "csync_exclude.h"

#include "common/utility.h"
#include "../version.h"

#include <QString>
#include <QFileInfo>
#include <QDir>


/** Expands C-like escape sequences (in place)
 */
OCSYNC_EXPORT void csync_exclude_expand_escapes(QByteArray &input)
{
    size_t o = 0;
    char *line = input.data();
    auto len = input.size();
    for (int i = 0; i < len; ++i) {
        if (line[i] == '\\') {
            // at worst input[i+1] is \0
            switch (line[i+1]) {
            case '\'': line[o++] = '\''; break;
            case '"': line[o++] = '"'; break;
            case '?': line[o++] = '?'; break;
            case '#': line[o++] = '#'; break;
            case 'a': line[o++] = '\a'; break;
            case 'b': line[o++] = '\b'; break;
            case 'f': line[o++] = '\f'; break;
            case 'n': line[o++] = '\n'; break;
            case 'r': line[o++] = '\r'; break;
            case 't': line[o++] = '\t'; break;
            case 'v': line[o++] = '\v'; break;
            default:
                // '\*' '\?' '\[' '\\' will be processed during regex translation
                // '\\' is intentionally not expanded here (to avoid '\\*' and '\*'
                // ending up meaning the same thing)
                line[o++] = line[i];
                line[o++] = line[i + 1];
                break;
            }
            ++i;
        } else {
            line[o++] = line[i];
        }
    }
    input.resize(OCC::Utility::convertSizeToInt(o));
}

// See http://support.microsoft.com/kb/74496 and
// https://msdn.microsoft.com/en-us/library/windows/desktop/aa365247(v=vs.85).aspx
// Additionally, we ignore '$Recycle.Bin', see https://github.com/owncloud/client/issues/2955
static const char *win_reserved_words_3[] = { "CON", "PRN", "AUX", "NUL" };
static const char *win_reserved_words_4[] = {
    "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
    "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
};
static const char *win_reserved_words_n[] = { "CLOCK$", "$Recycle.Bin" };

/**
 * @brief Checks if filename is considered reserved by Windows
 * @param file_name filename
 * @return true if file is reserved, false otherwise
 */
OCSYNC_EXPORT bool csync_is_windows_reserved_word(const QStringRef &filename)
{
    size_t len_filename = filename.size();

    // Drive letters
    if (len_filename == 2 && filename.at(1) == QLatin1Char(':')) {
        if (filename.at(0) >= QLatin1Char('a') && filename.at(0) <= QLatin1Char('z')) {
            return true;
        }
        if (filename.at(0) >= QLatin1Char('A') && filename.at(0) <= QLatin1Char('Z')) {
            return true;
        }
    }

    if (len_filename == 3 || (len_filename > 3 && filename.at(3) == QLatin1Char('.'))) {
        for (const char *word : win_reserved_words_3) {
            if (filename.left(3).compare(QLatin1String(word), Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
    }

    if (len_filename == 4 || (len_filename > 4 && filename.at(4) == QLatin1Char('.'))) {
        for (const char *word : win_reserved_words_4) {
            if (filename.left(4).compare(QLatin1String(word), Qt::CaseInsensitive) == 0) {
                return true;
            }
        }
    }

    for (const char *word : win_reserved_words_n) {
        if (filename.compare(QLatin1String(word), Qt::CaseInsensitive) == 0) {
            return true;
        }
    }

    return false;
}

static CSYNC_EXCLUDE_TYPE _csync_excluded_common(const QString &path, bool excludeConflictFiles)
{
    /* split up the path */
    QStringRef bname(&path);
    int lastSlash = path.lastIndexOf(QLatin1Char('/'));
    if (lastSlash >= 0) {
        bname = path.midRef(lastSlash + 1);
    }

    qsizetype blen = bname.size();
    // 9 = strlen(".sync_.db")
    if (blen >= 9 && bname.at(0) == QLatin1Char('.')) {
        if (bname.contains(QLatin1String(".db"))) {
            if (bname.startsWith(QLatin1String("._sync_"), Qt::CaseInsensitive)  // "._sync_*.db*"
                || bname.startsWith(QLatin1String(".sync_"), Qt::CaseInsensitive) // ".sync_*.db*"
                || bname.startsWith(QLatin1String(".csync_journal.db"), Qt::CaseInsensitive)) { // ".csync_journal.db*"
                return CSYNC_FILE_SILENTLY_EXCLUDED;
            }
        }
        if (bname.startsWith(QLatin1String(".owncloudsync.log"), Qt::CaseInsensitive)) { // ".owncloudsync.log*"
            return CSYNC_FILE_SILENTLY_EXCLUDED;
        }
    }

    // check the strlen and ignore the file if its name is longer than 254 chars.
    // whenever changing this also check createDownloadTmpFileName
    if (blen > 254) {
        return CSYNC_FILE_EXCLUDE_LONG_FILENAME;
    }

#ifdef _WIN32
    // Windows cannot sync files ending in spaces (#2176). It also cannot
    // distinguish files ending in '.' from files without an ending,
    // as '.' is a separator that is not stored internally, so let's
    // not allow to sync those to avoid file loss/ambiguities (#416)
    if (blen > 1) {
        if (bname.at(blen - 1) == QLatin1Char(' ')) {
            return CSYNC_FILE_EXCLUDE_TRAILING_SPACE;
        } else if (bname.at(blen - 1) == QLatin1Char('.')) {
            return CSYNC_FILE_EXCLUDE_INVALID_CHAR;
        }
    }

    if (csync_is_windows_reserved_word(bname)) {
        return CSYNC_FILE_EXCLUDE_INVALID_CHAR;
    }

    // Filter out characters not allowed in a filename on windows
    for (auto p : path) {
        const ushort c = p.unicode();
        if (c < 32) {
            return CSYNC_FILE_EXCLUDE_INVALID_CHAR;
        }
        switch (c) {
        case '\\':
        case ':':
        case '?':
        case '*':
        case '"':
        case '>':
        case '<':
        case '|':
            return CSYNC_FILE_EXCLUDE_INVALID_CHAR;
        default:
            break;
        }
    }
#endif

    /* We create a Desktop.ini on Windows for the sidebar icon, make sure we don't sync it. */
    if (blen == 11 && path == bname) {
        if (bname.compare(QLatin1String("Desktop.ini"), Qt::CaseInsensitive) == 0) {
            return CSYNC_FILE_SILENTLY_EXCLUDED;
        }
    }

    if (excludeConflictFiles && OCC::Utility::isConflictFile(path)) {
        return CSYNC_FILE_EXCLUDE_CONFLICT;
    }
    return CSYNC_NOT_EXCLUDED;
}

static QString leftIncludeLast(const QString &arr, const QChar &c)
{
    // left up to and including `c`
    return arr.left(arr.lastIndexOf(c, arr.size() - 2) + 1);
}

using namespace OCC;

ExcludedFiles::ExcludedFiles(const QString &localPath)
    : _localPath(localPath)
    , _clientVersion(MIRALL_VERSION_MAJOR, MIRALL_VERSION_MINOR, MIRALL_VERSION_PATCH)
{
    Q_ASSERT(_localPath.endsWith(QStringLiteral("/")));
    // Windows used to use PathMatchSpec which allows *foo to match abc/deffoo.
    _wildcardsMatchSlash = Utility::isWindows();

    // We're in a detached exclude probably coming from a partial sync or test
    if (_localPath.isEmpty())
        return;

    // Load exclude file from base dir
    QFileInfo fi(_localPath + QStringLiteral(".sync-exclude.lst"));
    if (fi.isReadable())
        addInTreeExcludeFilePath(fi.absoluteFilePath());
}

ExcludedFiles::~ExcludedFiles() = default;

void ExcludedFiles::addExcludeFilePath(const QString &path)
{
    _excludeFiles[_localPath].append(path);
}

void ExcludedFiles::addInTreeExcludeFilePath(const QString &path)
{
    BasePathString basePath = leftIncludeLast(path, QLatin1Char('/'));
    _excludeFiles[basePath].append(path);
}

void ExcludedFiles::setExcludeConflictFiles(bool onoff)
{
    _excludeConflictFiles = onoff;
}

void ExcludedFiles::addManualExclude(const QString &expr)
{
    addManualExclude(expr, _localPath);
}

void ExcludedFiles::addManualExclude(const QString &expr, const QString &basePath)
{
    Q_ASSERT(basePath.endsWith(QLatin1Char('/')));

    auto key = basePath;
    _manualExcludes[key].append(expr);
    _allExcludes[key].append(expr);
    prepare(key);
}

void ExcludedFiles::clearManualExcludes()
{
    _manualExcludes.clear();
    reloadExcludeFiles();
}

void ExcludedFiles::setWildcardsMatchSlash(bool onoff)
{
    _wildcardsMatchSlash = onoff;
    prepare();
}

void ExcludedFiles::setClientVersion(ExcludedFiles::Version version)
{
    _clientVersion = version;
}

void ExcludedFiles::loadExcludeFilePatterns(const QString &basePath, QFile &file)
{
    QStringList patterns;
    while (!file.atEnd()) {
        QByteArray line = file.readLine().trimmed();
        if (line.startsWith("#!version")) {
            if (!versionDirectiveKeepNextLine(line))
                file.readLine();
        }
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        csync_exclude_expand_escapes(line);
        patterns.append(QString::fromUtf8(line));
    }
    _allExcludes.insert(basePath, patterns);

    // nothing to prepare if the user decided to not exclude anything
    if (!_allExcludes.value(basePath).isEmpty()){
        prepare(basePath);
    }
}

bool ExcludedFiles::reloadExcludeFiles()
{
    _allExcludes.clear();
    // clear all regex
    _bnameTraversalRegexFile.clear();
    _bnameTraversalRegexDir.clear();
    _fullTraversalRegexFile.clear();
    _fullTraversalRegexDir.clear();
    _fullRegexFile.clear();
    _fullRegexDir.clear();

    bool success = true;
    const auto keys = _excludeFiles.keys();
    for (const auto& basePath : keys) {
        for (const auto& excludeFile : _excludeFiles.value(basePath)) {
            QFile file(excludeFile);
            if (file.open(QIODevice::ReadOnly)) {
                if (file.exists()) {
                    loadExcludeFilePatterns(basePath, file);
                } else {
                    success = false;
                    qWarning() << "System exclude list file doesn't exist:" << excludeFile;
                }
            }

        }
    }

    auto endManual = _manualExcludes.cend();
    for (auto kv = _manualExcludes.cbegin(); kv != endManual; ++kv) {
        _allExcludes[kv.key()].append(kv.value());
        prepare(kv.key());
    }

    return success;
}

bool ExcludedFiles::versionDirectiveKeepNextLine(const QByteArray &directive) const
{
    if (!directive.startsWith("#!version"))
        return true;
    QByteArrayList args = directive.split(' ');
    if (args.size() != 3)
        return true;
    QByteArray op = args[1];
    QByteArrayList argVersions = args[2].split('.');
    if (argVersions.size() != 3)
        return true;

    auto argVersion = std::make_tuple(argVersions[0].toInt(), argVersions[1].toInt(), argVersions[2].toInt());
    if (op == "<=")
        return _clientVersion <= argVersion;
    if (op == "<")
        return _clientVersion < argVersion;
    if (op == ">")
        return _clientVersion > argVersion;
    if (op == ">=")
        return _clientVersion >= argVersion;
    if (op == "==")
        return _clientVersion == argVersion;
    return true;
}

bool ExcludedFiles::isExcluded(
    const QString &filePath,
    const QString &basePath,
    bool excludeHidden) const
{
    if (!filePath.startsWith(basePath, Utility::fsCasePreserving() ? Qt::CaseInsensitive : Qt::CaseSensitive)) {
        // Mark paths we're not responsible for as excluded...
        return true;
    }

    //TODO this seems a waste, hidden files are ignored before hitting this function it seems
    if (excludeHidden) {
        QString path = filePath;
        // Check all path subcomponents, but to *not* check the base path:
        // We do want to be able to sync with a hidden folder as the target.
        while (path.size() > basePath.size()) {
            QFileInfo fi(path);
            if (fi.fileName() != QStringLiteral(".sync-exclude.lst")
                && (fi.isHidden() || fi.fileName().startsWith(QLatin1Char('.')))) {
                return true;
            }

            // Get the parent path
            path = fi.absolutePath();
        }
    }

    QFileInfo fi(filePath);
    ItemType type = ItemTypeFile;
    if (fi.isDir()) {
        type = ItemTypeDirectory;
    }

    QString relativePath = filePath.mid(basePath.size());
    if (relativePath.endsWith(QLatin1Char('/'))) {
        relativePath.chop(1);
    }

    return fullPatternMatch(relativePath, type) != CSYNC_NOT_EXCLUDED;
}

CSYNC_EXCLUDE_TYPE ExcludedFiles::traversalPatternMatch(const QString &path, ItemType filetype)
{
    auto match = _csync_excluded_common(path, _excludeConflictFiles);
    if (match != CSYNC_NOT_EXCLUDED)
        return match;
    if (_allExcludes.isEmpty())
        return CSYNC_NOT_EXCLUDED;

    // Directories are guaranteed to be visited before their files
    if (filetype == ItemTypeDirectory) {
        const auto basePath = QString(_localPath + path + QLatin1Char('/'));
        const QString absolutePath = basePath + QStringLiteral(".sync-exclude.lst");
        QFile excludeFile(absolutePath);

        if (excludeFile.open(QIODevice::ReadOnly)) {
            if (excludeFile.exists()) {
                addInTreeExcludeFilePath(absolutePath);
                loadExcludeFilePatterns(basePath, excludeFile);
            }
        }
    }

    // Check the bname part of the path to see whether the full
    // regex should be run.
    QStringRef bnameStr(&path);
    int lastSlash = path.lastIndexOf(QLatin1Char('/'));
    if (lastSlash >= 0) {
        bnameStr = path.midRef(lastSlash + 1);
    }

    QString basePath(_localPath + path);
    while (basePath.size() > _localPath.size()) {
        basePath = leftIncludeLast(basePath, QLatin1Char('/'));
        QRegularExpressionMatch m;
        if (filetype == ItemTypeDirectory
            && _bnameTraversalRegexDir.contains(basePath)) {
            m = _bnameTraversalRegexDir[basePath].match(bnameStr);
        } else if (filetype == ItemTypeFile
            && _bnameTraversalRegexFile.contains(basePath)) {
            m = _bnameTraversalRegexFile[basePath].match(bnameStr);
        } else {
            continue;
        }

        if (!m.hasMatch())
            return CSYNC_NOT_EXCLUDED;
        if (m.capturedStart(QStringLiteral("exclude")) != -1) {
            return CSYNC_FILE_EXCLUDE_LIST;
        } else if (m.capturedStart(QStringLiteral("excluderemove")) != -1) {
            return CSYNC_FILE_EXCLUDE_AND_REMOVE;
        }
    }

    // third capture: full path matching is triggered
    basePath = _localPath + path;
    while (basePath.size() > _localPath.size()) {
        basePath = leftIncludeLast(basePath, QLatin1Char('/'));
        QRegularExpressionMatch m;
        if (filetype == ItemTypeDirectory
            && _fullTraversalRegexDir.contains(basePath)) {
            m = _fullTraversalRegexDir[basePath].match(path);
        } else if (filetype == ItemTypeFile
            && _fullTraversalRegexFile.contains(basePath)) {
            m = _fullTraversalRegexFile[basePath].match(path);
        } else {
            continue;
        }

        if (m.hasMatch()) {
            if (m.capturedStart(QStringLiteral("exclude")) != -1) {
                return CSYNC_FILE_EXCLUDE_LIST;
            } else if (m.capturedStart(QStringLiteral("excluderemove")) != -1) {
                return CSYNC_FILE_EXCLUDE_AND_REMOVE;
            }
        }
    }
    return CSYNC_NOT_EXCLUDED;
}

CSYNC_EXCLUDE_TYPE ExcludedFiles::fullPatternMatch(const QString &p, ItemType filetype) const
{
    auto match = _csync_excluded_common(p, _excludeConflictFiles);
    if (match != CSYNC_NOT_EXCLUDED)
        return match;
    if (_allExcludes.isEmpty())
        return CSYNC_NOT_EXCLUDED;

    // `path` seems to always be relative to `_localPath`, the tests however have not been
    // written that way... this makes the tests happy for now. TODO Fix the tests at some point
    QString path = p;
    if (path.startsWith(_localPath))
        path = path.mid(_localPath.size());

    QString basePath(_localPath + path);
    while (basePath.size() > _localPath.size()) {
        basePath = leftIncludeLast(basePath, QLatin1Char('/'));
        QRegularExpressionMatch m;
        if (filetype == ItemTypeDirectory
            && _fullRegexDir.contains(basePath)) {
            m = _fullRegexDir[basePath].match(p);
        } else if (filetype == ItemTypeFile
            && _fullRegexFile.contains(basePath)) {
            m = _fullRegexFile[basePath].match(p);
        } else {
            continue;
        }

        if (m.hasMatch()) {
            if (m.capturedStart(QStringLiteral("exclude")) != -1) {
                return CSYNC_FILE_EXCLUDE_LIST;
            } else if (m.capturedStart(QStringLiteral("excluderemove")) != -1) {
                return CSYNC_FILE_EXCLUDE_AND_REMOVE;
            }
        }
    }

    return CSYNC_NOT_EXCLUDED;
}

/**
 * On linux we used to use fnmatch with FNM_PATHNAME, but the windows function we used
 * didn't have that behavior. wildcardsMatchSlash can be used to control which behavior
 * the resulting regex shall use.
 */
QString ExcludedFiles::convertToRegexpSyntax(QString exclude, bool wildcardsMatchSlash)
{
    // Translate *, ?, [...] to their regex variants.
    // The escape sequences \*, \?, \[. \\ have a special meaning,
    // the other ones have already been expanded before
    // (like "\\n" being replaced by "\n").
    //
    // QString being UTF-16 makes unicode-correct escaping tricky.
    // If we escaped each UTF-16 code unit we'd end up splitting 4-byte
    // code points. To avoid problems we delegate as much work as possible to
    // QRegularExpression::escape(): It always receives as long a sequence
    // as code units as possible.
    QString regex;
    int i = 0;
    int charsToEscape = 0;
    auto flush = [&]() {
        regex.append(QRegularExpression::escape(exclude.mid(i - charsToEscape, charsToEscape)));
        charsToEscape = 0;
    };
    auto len = exclude.size();
    for (; i < len; ++i) {
        switch (exclude[i].unicode()) {
        case '*':
            flush();
            if (wildcardsMatchSlash) {
                regex.append(QLatin1String(".*"));
            } else {
                regex.append(QLatin1String("[^/]*"));
            }
            break;
        case '?':
            flush();
            if (wildcardsMatchSlash) {
                regex.append(QLatin1Char('.'));
            } else {
                regex.append(QStringLiteral("[^/]"));
            }
            break;
        case '[': {
            flush();
            // Find the end of the bracket expression
            auto j = i + 1;
            for (; j < len; ++j) {
                if (exclude[j] == QLatin1Char(']'))
                    break;
                if (j != len - 1 && exclude[j] == QLatin1Char('\\') && exclude[j + 1] == QLatin1Char(']'))
                    ++j;
            }
            if (j == len) {
                // no matching ], just insert the escaped [
                regex.append(QStringLiteral("\\["));
                break;
            }
            // Translate [! to [^
            QString bracketExpr = exclude.mid(i, j - i + 1);
            if (bracketExpr.startsWith(QLatin1String("[!")))
                bracketExpr[1] = QLatin1Char('^');
            regex.append(bracketExpr);
            i = j;
            break;
        }
        case '\\':
            flush();
            if (i == len - 1) {
                regex.append(QStringLiteral("\\\\"));
                break;
            }
            // '\*' -> '\*', but '\z' -> '\\z'
            switch (exclude[i + 1].unicode()) {
            case '*':
            case '?':
            case '[':
            case '\\':
                regex.append(QRegularExpression::escape(exclude.mid(i + 1, 1)));
                break;
            default:
                charsToEscape += 2;
                break;
            }
            ++i;
            break;
        default:
            ++charsToEscape;
            break;
        }
    }
    flush();
    return regex;
}

QString ExcludedFiles::extractBnameTrigger(const QString &exclude, bool wildcardsMatchSlash)
{
    // We can definitely drop everything to the left of a / - that will never match
    // any bname.
    QString pattern = exclude.mid(exclude.lastIndexOf(QLatin1Char('/')) + 1);

    // Easy case, nothing else can match a slash, so that's it.
    if (!wildcardsMatchSlash)
        return pattern;

    // Otherwise it's more complicated. Examples:
    // - "foo*bar" can match "fooX/Xbar", pattern is "*bar"
    // - "foo*bar*" can match "fooX/XbarX", pattern is "*bar*"
    // - "foo?bar" can match "foo/bar" but also "fooXbar", pattern is "*bar"

    auto isWildcard = [](QChar c) { return c == QLatin1Char('*') || c == QLatin1Char('?'); };

    // First, skip wildcards on the very right of the pattern
    int i = pattern.size() - 1;
    while (i >= 0 && isWildcard(pattern[i]))
        --i;

    // Then scan further until the next wildcard that could match a /
    while (i >= 0 && !isWildcard(pattern[i]))
        --i;

    // Everything to the right is part of the pattern
    pattern = pattern.mid(i + 1);

    // And if there was a wildcard, it starts with a *
    if (i >= 0)
        pattern.prepend(QLatin1Char('*'));

    return pattern;
}

void ExcludedFiles::prepare()
{
    // clear all regex
    _bnameTraversalRegexFile.clear();
    _bnameTraversalRegexDir.clear();
    _fullTraversalRegexFile.clear();
    _fullTraversalRegexDir.clear();
    _fullRegexFile.clear();
    _fullRegexDir.clear();

    const auto keys = _allExcludes.keys();
    for (auto const & basePath : keys)
        prepare(basePath);
}

void ExcludedFiles::prepare(const BasePathString & basePath)
{
    Q_ASSERT(_allExcludes.contains(basePath));

    // Build regular expressions for the different cases.
    //
    // To compose the _bnameTraversalRegex, _fullTraversalRegex and _fullRegex
    // patterns we collect several subgroups of patterns here.
    //
    // * The "full" group will contain all patterns that contain a non-trailing
    //   slash. They only make sense in the fullRegex and fullTraversalRegex.
    // * The "bname" group contains all patterns without a non-trailing slash.
    //   These need separate handling in the _fullRegex (slash-containing
    //   patterns must be anchored to the front, these don't need it)
    // * The "bnameTrigger" group contains the bname part of all patterns in the
    //   "full" group. These and the "bname" group become _bnameTraversalRegex.
    //
    // To complicate matters, the exclude patterns have two binary attributes
    // meaning we'll end up with 4 variants:
    // * "]" patterns mean "EXCLUDE_AND_REMOVE", they get collected in the
    //   pattern strings ending in "Remove". The others go to "Keep".
    // * trailing-slash patterns match directories only. They get collected
    //   in the pattern strings saying "Dir", the others go into "FileDir"
    //   because they match files and directories.

    QString fullFileDirKeep;
    QString fullFileDirRemove;
    QString fullDirKeep;
    QString fullDirRemove;

    QString bnameFileDirKeep;
    QString bnameFileDirRemove;
    QString bnameDirKeep;
    QString bnameDirRemove;

    QString bnameTriggerFileDir;
    QString bnameTriggerDir;

    auto regexAppend = [](QString &fileDirPattern, QString &dirPattern, const QString &appendMe, bool dirOnly) {
        QString &pattern = dirOnly ? dirPattern : fileDirPattern;
        if (!pattern.isEmpty())
            pattern.append(QLatin1Char('|'));
        pattern.append(appendMe);
    };

    for (auto exclude : _allExcludes.value(basePath)) {
        if (exclude[0] == QLatin1Char('\n'))
            continue; // empty line
        if (exclude[0] == QLatin1Char('\r'))
            continue; // empty line

        bool matchDirOnly = exclude.endsWith(QLatin1Char('/'));
        if (matchDirOnly)
            exclude = exclude.left(exclude.size() - 1);

        bool removeExcluded = (exclude[0] == QLatin1Char(']'));
        if (removeExcluded)
            exclude = exclude.mid(1);

        bool fullPath = exclude.contains(QLatin1Char('/'));

        /* Use QRegularExpression, append to the right pattern */
        auto &bnameFileDir = removeExcluded ? bnameFileDirRemove : bnameFileDirKeep;
        auto &bnameDir = removeExcluded ? bnameDirRemove : bnameDirKeep;
        auto &fullFileDir = removeExcluded ? fullFileDirRemove : fullFileDirKeep;
        auto &fullDir = removeExcluded ? fullDirRemove : fullDirKeep;

        if (fullPath) {
            // The full pattern is matched against a path relative to _localPath, however exclude is
            // relative to basePath at this point.
            // We know for sure that both _localPath and basePath are absolute and that basePath is
            // contained in _localPath. So we can simply remove it from the begining.
            auto relPath = basePath.mid(_localPath.size());
            // Make exclude relative to _localPath
            exclude.prepend(relPath);
        }
        auto regexExclude = convertToRegexpSyntax(exclude, _wildcardsMatchSlash);
        if (!fullPath) {
            regexAppend(bnameFileDir, bnameDir, regexExclude, matchDirOnly);
        } else {
            regexAppend(fullFileDir, fullDir, regexExclude, matchDirOnly);

            // For activation, trigger on the 'bname' part of the full pattern.
            QString bnameExclude = extractBnameTrigger(exclude, _wildcardsMatchSlash);
            auto regexBname = convertToRegexpSyntax(bnameExclude, true);
            regexAppend(bnameTriggerFileDir, bnameTriggerDir, regexBname, matchDirOnly);
        }
    }

    // The empty pattern would match everything - change it to match-nothing
    auto emptyMatchNothing = [](QString &pattern) {
        if (pattern.isEmpty())
            pattern = QStringLiteral("a^");
    };
    emptyMatchNothing(fullFileDirKeep);
    emptyMatchNothing(fullFileDirRemove);
    emptyMatchNothing(fullDirKeep);
    emptyMatchNothing(fullDirRemove);

    emptyMatchNothing(bnameFileDirKeep);
    emptyMatchNothing(bnameFileDirRemove);
    emptyMatchNothing(bnameDirKeep);
    emptyMatchNothing(bnameDirRemove);

    emptyMatchNothing(bnameTriggerFileDir);
    emptyMatchNothing(bnameTriggerDir);

    // The bname regex is applied to the bname only, so it must be
    // anchored in the beginning and in the end. It has the structure:
    // (exclude)|(excluderemove)|(bname triggers).
    // If the third group matches, the fullActivatedRegex needs to be applied
    // to the full path.
    _bnameTraversalRegexFile[basePath].setPattern(
        QStringLiteral("^(?P<exclude>%1)$|"
                       "^(?P<excluderemove>%2)$|"
                       "^(?P<trigger>%3)$")
            .arg(bnameFileDirKeep, bnameFileDirRemove, bnameTriggerFileDir));
    _bnameTraversalRegexDir[basePath].setPattern(
        QStringLiteral("^(?P<exclude>%1|%2)$|"
                       "^(?P<excluderemove>%3|%4)$|"
                       "^(?P<trigger>%5|%6)$")
            .arg(bnameFileDirKeep, bnameDirKeep, bnameFileDirRemove, bnameDirRemove, bnameTriggerFileDir, bnameTriggerDir));

    // The full traveral regex is applied to the full path if the trigger capture of
    // the bname regex matches. Its basic form is (exclude)|(excluderemove)".
    // This pattern can be much simpler than fullRegex since we can assume a traversal
    // situation and doesn't need to look for bname patterns in parent paths.
    _fullTraversalRegexFile[basePath].setPattern(
        // Full patterns are anchored to the beginning
        QStringLiteral("^(?P<exclude>%1)(?:$|/)"
                       "|"
                       "^(?P<excluderemove>%2)(?:$|/)")
            .arg(fullFileDirKeep, fullFileDirRemove));
    _fullTraversalRegexDir[basePath].setPattern(
        QStringLiteral("^(?P<exclude>%1|%2)(?:$|/)"
                       "|"
                       "^(?P<excluderemove>%3|%4)(?:$|/)")
            .arg(fullFileDirKeep, fullDirKeep, fullFileDirRemove, fullDirRemove));

    // The full regex is applied to the full path and incorporates both bname and
    // full-path patterns. It has the form "(exclude)|(excluderemove)".
    _fullRegexFile[basePath].setPattern(
        QStringLiteral("(?P<exclude>"
                       // Full patterns are anchored to the beginning
                       "^(?:%1)(?:$|/)|"
                       // Simple bname patterns can be any path component
                       "(?:^|/)(?:%2)(?:$|/)|"
                       // When checking a file for exclusion we must check all parent paths
                       // against the dir-only patterns as well.
                       "(?:^|/)(?:%3)/)"
                       "|"
                       "(?P<excluderemove>"
                       "^(?:%4)(?:$|/)|"
                       "(?:^|/)(?:%5)(?:$|/)|"
                       "(?:^|/)(?:%6)/)")
            .arg(fullFileDirKeep, bnameFileDirKeep, bnameDirKeep, fullFileDirRemove, bnameFileDirRemove, bnameDirRemove));
    _fullRegexDir[basePath].setPattern(
        QStringLiteral("(?P<exclude>"
                       "^(?:%1|%2)(?:$|/)|"
                       "(?:^|/)(?:%3|%4)(?:$|/))"
                       "|"
                       "(?P<excluderemove>"
                       "^(?:%5|%6)(?:$|/)|"
                       "(?:^|/)(?:%7|%8)(?:$|/))")
            .arg(fullFileDirKeep, fullDirKeep, bnameFileDirKeep, bnameDirKeep, fullFileDirRemove, fullDirRemove, bnameFileDirRemove, bnameDirRemove));

    QRegularExpression::PatternOptions patternOptions = QRegularExpression::NoPatternOption;
    if (OCC::Utility::fsCasePreserving())
        patternOptions |= QRegularExpression::CaseInsensitiveOption;
    _bnameTraversalRegexFile[basePath].setPatternOptions(patternOptions);
    _bnameTraversalRegexFile[basePath].optimize();
    _bnameTraversalRegexDir[basePath].setPatternOptions(patternOptions);
    _bnameTraversalRegexDir[basePath].optimize();
    _fullTraversalRegexFile[basePath].setPatternOptions(patternOptions);
    _fullTraversalRegexFile[basePath].optimize();
    _fullTraversalRegexDir[basePath].setPatternOptions(patternOptions);
    _fullTraversalRegexDir[basePath].optimize();
    _fullRegexFile[basePath].setPatternOptions(patternOptions);
    _fullRegexFile[basePath].optimize();
    _fullRegexDir[basePath].setPatternOptions(patternOptions);
    _fullRegexDir[basePath].optimize();
}
