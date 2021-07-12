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

#include "folderstatusmodel.h"
#include "folderman.h"
#include "accountstate.h"
#include "common/asserts.h"
#include <theme.h>
#include <account.h>
#include "folderstatusdelegate.h"

#include <QFileIconProvider>
#include <QVarLengthArray>
#include <set>

Q_DECLARE_METATYPE(QPersistentModelIndex)

namespace OCC {

Q_LOGGING_CATEGORY(lcFolderStatus, "nextcloud.gui.folder.model", QtInfoMsg)

static const char propertyParentIndexC[] = "oc_parentIndex";
static const char propertyPermissionMap[] = "oc_permissionMap";
static const char propertyEncryptionMap[] = "nc_encryptionMap";

static QString removeTrailingSlash(const QString &s)
{
    if (s.endsWith('/')) {
        return s.left(s.size() - 1);
    }
    return s;
}

FolderStatusModel::FolderStatusModel(QObject *parent)
    : QAbstractItemModel(parent)
{

}

FolderStatusModel::~FolderStatusModel() = default;

static bool sortByFolderHeader(const FolderStatusModel::SubFolderInfo &lhs, const FolderStatusModel::SubFolderInfo &rhs)
{
    return QString::compare(lhs._folder->shortGuiRemotePathOrAppName(),
               rhs._folder->shortGuiRemotePathOrAppName(),
               Qt::CaseInsensitive)
        < 0;
}

void FolderStatusModel::setAccountState(const AccountState *accountState)
{
    beginResetModel();
    _dirty = false;
    _folders.clear();
    _accountState = accountState;

    connect(FolderMan::instance(), &FolderMan::folderSyncStateChange,
        this, &FolderStatusModel::slotFolderSyncStateChange, Qt::UniqueConnection);
    connect(FolderMan::instance(), &FolderMan::scheduleQueueChanged,
        this, &FolderStatusModel::slotFolderScheduleQueueChanged, Qt::UniqueConnection);

    auto folders = FolderMan::instance()->map();
    foreach (auto f, folders) {
        if (!accountState)
            break;
        if (f->accountState() != accountState)
            continue;
        SubFolderInfo info;
        info._name = f->alias();
        info._path = "/";
        info._folder = f;
        info._checked = Qt::PartiallyChecked;
        _folders << info;

        connect(f, &Folder::progressInfo, this, &FolderStatusModel::slotSetProgress, Qt::UniqueConnection);
        connect(f, &Folder::newBigFolderDiscovered, this, &FolderStatusModel::slotNewBigFolder, Qt::UniqueConnection);
    }

    // Sort by header text
    std::sort(_folders.begin(), _folders.end(), sortByFolderHeader);

    // Set the root _pathIdx after the sorting
    for (int i = 0; i < _folders.size(); ++i) {
        _folders[i]._pathIdx << i;
    }

    endResetModel();
    emit dirtyChanged();
}


Qt::ItemFlags FolderStatusModel::flags(const QModelIndex &index) const
{
    if (!_accountState) {
        return {};
    }

    const auto info = infoForIndex(index);
    const auto supportsSelectiveSync = info && info->_folder && info->_folder->supportsSelectiveSync();

    switch (classify(index)) {
    case AddButton: {
        Qt::ItemFlags ret;
        ret = Qt::ItemNeverHasChildren;
        if (!_accountState->isConnected()) {
            return ret;
        }
        return Qt::ItemIsEnabled | ret;
    }
    case FetchLabel:
        return Qt::ItemIsEnabled | Qt::ItemNeverHasChildren;
    case RootFolder:
        return Qt::ItemIsEnabled;
    case SubFolder:
        if (supportsSelectiveSync) {
            return Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsSelectable;
        } else {
            return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
        }
    }
    return {};
}

QVariant FolderStatusModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return QVariant();

    if (role == Qt::EditRole)
        return QVariant();

    switch (classify(index)) {
    case AddButton: {
        if (role == FolderStatusDelegate::AddButton) {
            return QVariant(true);
        } else if (role == Qt::ToolTipRole) {
            if (!_accountState->isConnected()) {
                return tr("You need to be connected to add a folder");
            }
            return tr("Click this button to add a folder to synchronize.");
        }
        return QVariant();
    }
    case SubFolder: {
        const auto &x = static_cast<SubFolderInfo *>(index.internalPointer())->_subs.at(index.row());
        const auto supportsSelectiveSync = x._folder && x._folder->supportsSelectiveSync();

        switch (role) {
        case Qt::DisplayRole:
            //: Example text: "File.txt (23KB)"
            return x._size < 0 ? x._name : tr("%1 (%2)").arg(x._name, Utility::octetsToString(x._size));
        case Qt::ToolTipRole:
            return QString(QLatin1String("<qt>") + Utility::escape(x._size < 0 ? x._name : tr("%1 (%2)").arg(x._name, Utility::octetsToString(x._size))) + QLatin1String("</qt>"));
        case Qt::CheckStateRole:
            if (supportsSelectiveSync) {
                return x._checked;
            } else {
                return QVariant();
            }
        case Qt::DecorationRole: {
            if (x._isEncrypted) {
                return QIcon(QLatin1String(":/client/theme/lock-https.svg"));
            } else if (x._size > 0 && isAnyAncestorEncrypted(index)) {
                return QIcon(QLatin1String(":/client/theme/lock-broken.svg"));
            }
            return QFileIconProvider().icon(x._isExternal ? QFileIconProvider::Network : QFileIconProvider::Folder);
        }
        case Qt::ForegroundRole:
            if (x._isUndecided) {
                return QColor(Qt::red);
            }
            break;
        case FileIdRole:
            return x._fileId;
        case FolderStatusDelegate::FolderPathRole: {
            auto f = x._folder;
            if (!f)
                return QVariant();
            return QVariant(f->path() + x._path);
        }
        }
    }
        return QVariant();
    case FetchLabel: {
        const auto x = static_cast<SubFolderInfo *>(index.internalPointer());
        switch (role) {
        case Qt::DisplayRole:
            if (x->_hasError) {
                return QVariant(tr("Error while loading the list of folders from the server.")
                    + QString("\n") + x->_lastErrorString);
            } else {
                return tr("Fetching folder list from server …");
            }
            break;
        default:
            return QVariant();
        }
    }
    case RootFolder:
        break;
    }

    const SubFolderInfo &folderInfo = _folders.at(index.row());
    auto f = folderInfo._folder;
    if (!f)
        return QVariant();

    const SubFolderInfo::Progress &progress = folderInfo._progress;
    const bool accountConnected = _accountState->isConnected();

    switch (role) {
    case FolderStatusDelegate::FolderPathRole:
        return f->shortGuiLocalPath();
    case FolderStatusDelegate::FolderSecondPathRole:
        return f->remotePath();
    case FolderStatusDelegate::FolderConflictMsg:
        return (f->syncResult().hasUnresolvedConflicts())
            ? QStringList(tr("There are unresolved conflicts. Click for details."))
            : QStringList();
    case FolderStatusDelegate::FolderErrorMsg:
        return f->syncResult().errorStrings();
    case FolderStatusDelegate::FolderInfoMsg:
        return f->virtualFilesEnabled() && f->vfs().mode() != Vfs::Mode::WindowsCfApi
            ? QStringList(tr("Virtual file support is enabled."))
            : QStringList();
    case FolderStatusDelegate::SyncRunning:
        return f->syncResult().status() == SyncResult::SyncRunning;
    case FolderStatusDelegate::SyncDate:
        return f->syncResult().syncTime();
    case FolderStatusDelegate::HeaderRole:
        return f->shortGuiRemotePathOrAppName();
    case FolderStatusDelegate::FolderAliasRole:
        return f->alias();
    case FolderStatusDelegate::FolderSyncPaused:
        return f->syncPaused();
    case FolderStatusDelegate::FolderAccountConnected:
        return accountConnected;
    case Qt::ToolTipRole: {
        QString toolTip;
        if (!progress.isNull()) {
            return progress._progressString;
        }
        if (accountConnected)
            toolTip = Theme::instance()->statusHeaderText(f->syncResult().status());
        else
            toolTip = tr("Signed out");
        toolTip += "\n";
        toolTip += folderInfo._folder->path();
        return toolTip;
    }
    case FolderStatusDelegate::FolderStatusIconRole:
        if (accountConnected) {
            auto theme = Theme::instance();
            auto status = f->syncResult().status();
            if (f->syncPaused()) {
                return theme->folderDisabledIcon();
            } else {
                if (status == SyncResult::SyncPrepare || status == SyncResult::Undefined) {
                    return theme->syncStateIcon(SyncResult::SyncRunning);
                } else {
                    // The "Problem" *result* just means some files weren't
                    // synced, so we show "Success" in these cases. But we
                    // do use the "Problem" *icon* for unresolved conflicts.
                    if (status == SyncResult::Success || status == SyncResult::Problem) {
                        if (f->syncResult().hasUnresolvedConflicts()) {
                            return theme->syncStateIcon(SyncResult::Problem);
                        } else {
                            return theme->syncStateIcon(SyncResult::Success);
                        }
                    } else {
                        return theme->syncStateIcon(status);
                    }
                }
            }
        } else {
            return Theme::instance()->folderOfflineIcon();
        }
    case FolderStatusDelegate::SyncProgressItemString:
        return progress._progressString;
    case FolderStatusDelegate::WarningCount:
        return progress._warningCount;
    case FolderStatusDelegate::SyncProgressOverallPercent:
        return progress._overallPercent;
    case FolderStatusDelegate::SyncProgressOverallString:
        return progress._overallSyncString;
    case FolderStatusDelegate::FolderSyncText:
        if (f->virtualFilesEnabled()) {
            return tr("Synchronizing VirtualFiles with local folder");
        } else {
            return tr("Synchronizing with local folder");
        }
    }
    return QVariant();
}

bool FolderStatusModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role == Qt::CheckStateRole) {
        auto info = infoForIndex(index);
        Q_ASSERT(info->_folder && info->_folder->supportsSelectiveSync());
        auto checked = static_cast<Qt::CheckState>(value.toInt());

        if (info && info->_checked != checked) {
            info->_checked = checked;
            if (checked == Qt::Checked) {
                // If we are checked, check that we may need to check the parent as well if
                // all the siblings are also checked
                QModelIndex parent = index.parent();
                auto parentInfo = infoForIndex(parent);
                if (parentInfo && parentInfo->_checked != Qt::Checked) {
                    bool hasUnchecked = false;
                    foreach (const auto &sub, parentInfo->_subs) {
                        if (sub._checked != Qt::Checked) {
                            hasUnchecked = true;
                            break;
                        }
                    }
                    if (!hasUnchecked) {
                        setData(parent, Qt::Checked, Qt::CheckStateRole);
                    } else if (parentInfo->_checked == Qt::Unchecked) {
                        setData(parent, Qt::PartiallyChecked, Qt::CheckStateRole);
                    }
                }
                // also check all the children
                for (int i = 0; i < info->_subs.count(); ++i) {
                    if (info->_subs.at(i)._checked != Qt::Checked) {
                        setData(this->index(i, 0, index), Qt::Checked, Qt::CheckStateRole);
                    }
                }
            }

            if (checked == Qt::Unchecked) {
                QModelIndex parent = index.parent();
                auto parentInfo = infoForIndex(parent);
                if (parentInfo && parentInfo->_checked == Qt::Checked) {
                    setData(parent, Qt::PartiallyChecked, Qt::CheckStateRole);
                }

                // Uncheck all the children
                for (int i = 0; i < info->_subs.count(); ++i) {
                    if (info->_subs.at(i)._checked != Qt::Unchecked) {
                        setData(this->index(i, 0, index), Qt::Unchecked, Qt::CheckStateRole);
                    }
                }
            }

            if (checked == Qt::PartiallyChecked) {
                QModelIndex parent = index.parent();
                auto parentInfo = infoForIndex(parent);
                if (parentInfo && parentInfo->_checked != Qt::PartiallyChecked) {
                    setData(parent, Qt::PartiallyChecked, Qt::CheckStateRole);
                }
            }
        }
        _dirty = true;
        emit dirtyChanged();
        emit dataChanged(index, index, QVector<int>() << role);
        return true;
    }
    return QAbstractItemModel::setData(index, value, role);
}


int FolderStatusModel::columnCount(const QModelIndex &) const
{
    return 1;
}

int FolderStatusModel::rowCount(const QModelIndex &parent) const
{
    if (!parent.isValid()) {
        if (Theme::instance()->singleSyncFolder() && _folders.count() != 0) {
            // "Add folder" button not visible in the singleSyncFolder configuration.
            return _folders.count();
        }
        return _folders.count() + 1; // +1 for the "add folder" button
    }
    auto info = infoForIndex(parent);
    if (!info)
        return 0;
    if (info->hasLabel())
        return 1;
    return info->_subs.count();
}

FolderStatusModel::ItemType FolderStatusModel::classify(const QModelIndex &index) const
{
    if (auto sub = static_cast<SubFolderInfo *>(index.internalPointer())) {
        if (sub->hasLabel()) {
            return FetchLabel;
        } else {
            return SubFolder;
        }
    }
    if (index.row() < _folders.count()) {
        return RootFolder;
    }
    return AddButton;
}

FolderStatusModel::SubFolderInfo *FolderStatusModel::infoForIndex(const QModelIndex &index) const
{
    if (!index.isValid())
        return nullptr;
    if (auto parentInfo = static_cast<SubFolderInfo *>(index.internalPointer())) {
        if (parentInfo->hasLabel()) {
            return nullptr;
        }
        if (index.row() >= parentInfo->_subs.size()) {
            return nullptr;
        }
        return &parentInfo->_subs[index.row()];
    } else {
        if (index.row() >= _folders.count()) {
            // AddButton
            return nullptr;
        }
        return const_cast<SubFolderInfo *>(&_folders[index.row()]);
    }
}

bool FolderStatusModel::isAnyAncestorEncrypted(const QModelIndex &index) const
{
    auto parentIndex = parent(index);
    while (parentIndex.isValid()) {
        const auto info = infoForIndex(parentIndex);
        if (info->_isEncrypted) {
            return true;
        }
        parentIndex = parent(parentIndex);
    }

    return false;
}

QModelIndex FolderStatusModel::indexForPath(Folder *f, const QString &path) const
{
    if (!f) {
        return {};
    }

    int slashPos = path.lastIndexOf('/');
    if (slashPos == -1) {
        // first level folder
        for (int i = 0; i < _folders.size(); ++i) {
            auto &info = _folders.at(i);
            if (info._folder == f) {
                if (path.isEmpty()) { // the folder object
                    return index(i, 0);
                }
                for (int j = 0; j < info._subs.size(); ++j) {
                    const QString subName = info._subs.at(j)._name;
                    if (subName == path) {
                        return index(j, 0, index(i));
                    }
                }
                return {};
            }
        }
        return {};
    }

    auto parent = indexForPath(f, path.left(slashPos));
    if (!parent.isValid())
        return parent;

    if (slashPos == path.size() - 1) {
        // The slash is the last part, we found our index
        return parent;
    }

    auto parentInfo = infoForIndex(parent);
    if (!parentInfo) {
        return {};
    }
    for (int i = 0; i < parentInfo->_subs.size(); ++i) {
        if (parentInfo->_subs.at(i)._name == path.mid(slashPos + 1)) {
            return index(i, 0, parent);
        }
    }

    return {};
}

QModelIndex FolderStatusModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!parent.isValid()) {
        return createIndex(row, column /*, nullptr*/);
    }
    switch (classify(parent)) {
    case AddButton:
    case FetchLabel:
        return {};
    case RootFolder:
        if (_folders.count() <= parent.row())
            return {}; // should not happen
        return createIndex(row, column, const_cast<SubFolderInfo *>(&_folders[parent.row()]));
    case SubFolder: {
        auto pinfo = static_cast<SubFolderInfo *>(parent.internalPointer());
        if (pinfo->_subs.count() <= parent.row())
            return {}; // should not happen
        auto &info = pinfo->_subs[parent.row()];
        if (!info.hasLabel()
            && info._subs.count() <= row)
            return {}; // should not happen
        return createIndex(row, column, &info);
    }
    }
    return {};
}

QModelIndex FolderStatusModel::parent(const QModelIndex &child) const
{
    if (!child.isValid()) {
        return {};
    }
    switch (classify(child)) {
    case RootFolder:
    case AddButton:
        return {};
    case SubFolder:
    case FetchLabel:
        break;
    }
    auto pathIdx = static_cast<SubFolderInfo *>(child.internalPointer())->_pathIdx;
    int i = 1;
    ASSERT(pathIdx.at(0) < _folders.count());
    if (pathIdx.count() == 1) {
        return createIndex(pathIdx.at(0), 0 /*, nullptr*/);
    }

    const SubFolderInfo *info = &_folders[pathIdx.at(0)];
    while (i < pathIdx.count() - 1) {
        ASSERT(pathIdx.at(i) < info->_subs.count());
        info = &info->_subs.at(pathIdx.at(i));
        ++i;
    }
    return createIndex(pathIdx.at(i), 0, const_cast<SubFolderInfo *>(info));
}

bool FolderStatusModel::hasChildren(const QModelIndex &parent) const
{
    if (!parent.isValid())
        return true;

    auto info = infoForIndex(parent);
    if (!info)
        return false;

    if (!info->_fetched)
        return true;

    if (info->_subs.isEmpty())
        return false;

    return true;
}


bool FolderStatusModel::canFetchMore(const QModelIndex &parent) const
{
    if (!_accountState) {
        return false;
    }
    if (_accountState->state() != AccountState::Connected) {
        return false;
    }
    auto info = infoForIndex(parent);
    if (!info || info->_fetched || info->_fetchingJob)
        return false;
    if (info->_hasError) {
        // Keep showing the error to the user, it will be hidden when the account reconnects
        return false;
    }
    return true;
}


void FolderStatusModel::fetchMore(const QModelIndex &parent)
{
    auto info = infoForIndex(parent);

    if (!info || info->_fetched || info->_fetchingJob)
        return;
    info->resetSubs(this, parent);
    QString path = info->_folder->remotePathTrailingSlash();

    // info->_path always contains non-mangled name, so we need to use mangled when requesting nested folders for encrypted subfolders as required by LsColJob
    const QString infoPath = (info->_isEncrypted && !info->_e2eMangledName.isEmpty()) ? info->_e2eMangledName : info->_path;

    if (infoPath != QLatin1String("/")) {
        path += infoPath;
    }

    auto *job = new LsColJob(_accountState->account(), path, this);
    info->_fetchingJob = job;
    auto props = QList<QByteArray>() << "resourcetype"
                                     << "http://owncloud.org/ns:size"
                                     << "http://owncloud.org/ns:permissions"
                                     << "http://owncloud.org/ns:fileid";
    if (_accountState->account()->capabilities().clientSideEncryptionAvailable()) {
        props << "http://nextcloud.org/ns:is-encrypted";
    }
    job->setProperties(props);

    job->setTimeout(60 * 1000);
    connect(job, &LsColJob::directoryListingSubfolders,
        this, &FolderStatusModel::slotUpdateDirectories);
    connect(job, &LsColJob::finishedWithError,
        this, &FolderStatusModel::slotLscolFinishedWithError);
    connect(job, &LsColJob::directoryListingIterated,
        this, &FolderStatusModel::slotGatherPermissions);
    connect(job, &LsColJob::directoryListingIterated,
            this, &FolderStatusModel::slotGatherEncryptionStatus);

    job->start();

    QPersistentModelIndex persistentIndex(parent);
    job->setProperty(propertyParentIndexC, QVariant::fromValue(persistentIndex));

    // Show 'fetching data...' hint after a while.
    _fetchingItems[persistentIndex].start();
    QTimer::singleShot(1000, this, &FolderStatusModel::slotShowFetchProgress);
}

void FolderStatusModel::resetAndFetch(const QModelIndex &parent)
{
    auto info = infoForIndex(parent);
    info->resetSubs(this, parent);
    fetchMore(parent);
}

void FolderStatusModel::slotGatherPermissions(const QString &href, const QMap<QString, QString> &map)
{
    auto it = map.find("permissions");
    if (it == map.end())
        return;

    auto job = sender();
    auto permissionMap = job->property(propertyPermissionMap).toMap();
    job->setProperty(propertyPermissionMap, QVariant()); // avoid a detach of the map while it is modified
    ASSERT(!href.endsWith(QLatin1Char('/')), "LsColXMLParser::parse should remove the trailing slash before calling us.");
    permissionMap[href] = *it;
    job->setProperty(propertyPermissionMap, permissionMap);
}

void FolderStatusModel::slotGatherEncryptionStatus(const QString &href, const QMap<QString, QString> &properties)
{
    auto it = properties.find("is-encrypted");
    if (it == properties.end())
        return;

    auto job = sender();
    auto encryptionMap = job->property(propertyEncryptionMap).toMap();
    job->setProperty(propertyEncryptionMap, QVariant()); // avoid a detach of the map while it is modified
    ASSERT(!href.endsWith(QLatin1Char('/')), "LsColXMLParser::parse should remove the trailing slash before calling us.");
    encryptionMap[href] = *it;
    job->setProperty(propertyEncryptionMap, encryptionMap);
}

void FolderStatusModel::slotUpdateDirectories(const QStringList &list)
{
    auto job = qobject_cast<LsColJob *>(sender());
    ASSERT(job);
    QModelIndex idx = qvariant_cast<QPersistentModelIndex>(job->property(propertyParentIndexC));
    auto parentInfo = infoForIndex(idx);
    if (!parentInfo) {
        return;
    }
    ASSERT(parentInfo->_fetchingJob == job);
    ASSERT(parentInfo->_subs.isEmpty());

    if (parentInfo->hasLabel()) {
        beginRemoveRows(idx, 0, 0);
        parentInfo->_hasError = false;
        parentInfo->_fetchingLabel = false;
        endRemoveRows();
    }

    parentInfo->_lastErrorString.clear();
    parentInfo->_fetchingJob = nullptr;
    parentInfo->_fetched = true;

    QUrl url = parentInfo->_folder->remoteUrl();
    QString pathToRemove = url.path();
    if (!pathToRemove.endsWith('/'))
        pathToRemove += '/';

    QStringList selectiveSyncBlackList;
    bool ok1 = true;
    bool ok2 = true;
    if (parentInfo->_checked == Qt::PartiallyChecked) {
        selectiveSyncBlackList = parentInfo->_folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, &ok1);
    }
    auto selectiveSyncUndecidedList = parentInfo->_folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncUndecidedList, &ok2);

    if (!(ok1 && ok2)) {
        qCWarning(lcFolderStatus) << "Could not retrieve selective sync info from journal";
        return;
    }

    std::set<QString> selectiveSyncUndecidedSet; // not QSet because it's not sorted
    foreach (const QString &str, selectiveSyncUndecidedList) {
        if (str.startsWith(parentInfo->_path) || parentInfo->_path == QLatin1String("/")) {
            selectiveSyncUndecidedSet.insert(str);
        }
    }
    const auto permissionMap = job->property(propertyPermissionMap).toMap();
    const auto encryptionMap = job->property(propertyEncryptionMap).toMap();

    QStringList sortedSubfolders = list;
    if (!sortedSubfolders.isEmpty())
        sortedSubfolders.removeFirst(); // skip the parent item (first in the list)
    Utility::sortFilenames(sortedSubfolders);

    QVarLengthArray<int, 10> undecidedIndexes;

    QVector<SubFolderInfo> newSubs;
    newSubs.reserve(sortedSubfolders.size());
    foreach (const QString &path, sortedSubfolders) {
        auto relativePath = path.mid(pathToRemove.size());
        if (parentInfo->_folder->isFileExcludedRelative(relativePath)) {
            continue;
        }

        SubFolderInfo newInfo;
        newInfo._folder = parentInfo->_folder;
        newInfo._pathIdx = parentInfo->_pathIdx;
        newInfo._pathIdx << newSubs.size();
        newInfo._isExternal = permissionMap.value(removeTrailingSlash(path)).toString().contains("M");
        newInfo._isEncrypted = encryptionMap.value(removeTrailingSlash(path)).toString() == QStringLiteral("1");
        newInfo._path = relativePath;

        SyncJournalFileRecord rec;
        parentInfo->_folder->journalDb()->getFileRecordByE2eMangledName(removeTrailingSlash(relativePath), &rec);
        if (rec.isValid()) {
            newInfo._name = removeTrailingSlash(rec._path).split('/').last();
            if (rec._isE2eEncrypted && !rec._e2eMangledName.isEmpty()) {
                // we must use local path for Settings Dialog's filesystem tree, otherwise open and create new folder actions won't work
                // hence, we are storing _e2eMangledName separately so it can be use later for LsColJob
                newInfo._e2eMangledName = relativePath;
                newInfo._path = rec._path;
            }
            if (!newInfo._path.endsWith('/')) {
                newInfo._path += '/';
            }
        } else {
            newInfo._name = removeTrailingSlash(relativePath).split('/').last();
        }

        const auto& folderInfo = job->_folderInfos.value(path);
        newInfo._size = folderInfo.size;
        newInfo._fileId = folderInfo.fileId;
        if (relativePath.isEmpty())
            continue;

        if (parentInfo->_checked == Qt::Unchecked) {
            newInfo._checked = Qt::Unchecked;
        } else if (parentInfo->_checked == Qt::Checked) {
            newInfo._checked = Qt::Checked;
        } else {
            foreach (const QString &str, selectiveSyncBlackList) {
                if (str == relativePath || str == QLatin1String("/")) {
                    newInfo._checked = Qt::Unchecked;
                    break;
                } else if (str.startsWith(relativePath)) {
                    newInfo._checked = Qt::PartiallyChecked;
                }
            }
        }

        auto it = selectiveSyncUndecidedSet.lower_bound(relativePath);
        if (it != selectiveSyncUndecidedSet.end()) {
            if (*it == relativePath) {
                newInfo._isUndecided = true;
                selectiveSyncUndecidedSet.erase(it);
            } else if ((*it).startsWith(relativePath)) {
                undecidedIndexes.append(newInfo._pathIdx.last());

                // Remove all the items from the selectiveSyncUndecidedSet that starts with this path
                QString relativePathNext = relativePath;
                relativePathNext[relativePathNext.length() - 1].unicode()++;
                auto it2 = selectiveSyncUndecidedSet.lower_bound(relativePathNext);
                selectiveSyncUndecidedSet.erase(it, it2);
            }
        }
        newSubs.append(newInfo);
    }

    if (!newSubs.isEmpty()) {
        beginInsertRows(idx, 0, newSubs.size() - 1);
        parentInfo->_subs = std::move(newSubs);
        endInsertRows();
    }

    for (int undecidedIndex : qAsConst(undecidedIndexes)) {
        suggestExpand(index(undecidedIndex, 0, idx));
    }
    /* Try to remove the the undecided lists the items that are not on the server. */
    auto it = std::remove_if(selectiveSyncUndecidedList.begin(), selectiveSyncUndecidedList.end(),
        [&](const QString &s) { return selectiveSyncUndecidedSet.count(s); });
    if (it != selectiveSyncUndecidedList.end()) {
        selectiveSyncUndecidedList.erase(it, selectiveSyncUndecidedList.end());
        parentInfo->_folder->journalDb()->setSelectiveSyncList(
            SyncJournalDb::SelectiveSyncUndecidedList, selectiveSyncUndecidedList);
        emit dirtyChanged();
    }
}

void FolderStatusModel::slotLscolFinishedWithError(QNetworkReply *r)
{
    auto job = qobject_cast<LsColJob *>(sender());
    ASSERT(job);
    QModelIndex idx = qvariant_cast<QPersistentModelIndex>(job->property(propertyParentIndexC));
    if (!idx.isValid()) {
        return;
    }
    auto parentInfo = infoForIndex(idx);
    if (parentInfo) {
        qCDebug(lcFolderStatus) << r->errorString();
        parentInfo->_lastErrorString = r->errorString();
        auto error = r->error();

        parentInfo->resetSubs(this, idx);

        if (error == QNetworkReply::ContentNotFoundError) {
            parentInfo->_fetched = true;
        } else {
            ASSERT(!parentInfo->hasLabel());
            beginInsertRows(idx, 0, 0);
            parentInfo->_hasError = true;
            endInsertRows();
        }
    }
}

QStringList FolderStatusModel::createBlackList(const FolderStatusModel::SubFolderInfo &root,
    const QStringList &oldBlackList) const
{
    switch (root._checked) {
    case Qt::Unchecked:
        return QStringList(root._path);
    case Qt::Checked:
        return QStringList();
    case Qt::PartiallyChecked:
        break;
    }

    QStringList result;
    if (root._fetched) {
        for (int i = 0; i < root._subs.count(); ++i) {
            result += createBlackList(root._subs.at(i), oldBlackList);
        }
    } else {
        // We did not load from the server so we re-use the one from the old black list
        const QString path = root._path;
        foreach (const QString &it, oldBlackList) {
            if (it.startsWith(path))
                result += it;
        }
    }
    return result;
}

void FolderStatusModel::slotUpdateFolderState(Folder *folder)
{
    if (!folder)
        return;
    for (int i = 0; i < _folders.count(); ++i) {
        if (_folders.at(i)._folder == folder) {
            emit dataChanged(index(i), index(i));
        }
    }
}

void FolderStatusModel::slotApplySelectiveSync()
{
    for (const auto &folderInfo : qAsConst(_folders)) {
        if (!folderInfo._fetched) {
            folderInfo._folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncUndecidedList, QStringList());
            continue;
        }
        const auto folder = folderInfo._folder;

        bool ok = false;
        auto oldBlackList = folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, &ok);
        if (!ok) {
            qCWarning(lcFolderStatus) << "Could not read selective sync list from db.";
            continue;
        }
        QStringList blackList = createBlackList(folderInfo, oldBlackList);
        folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, blackList);

        auto blackListSet = blackList.toSet();
        auto oldBlackListSet = oldBlackList.toSet();

        // The folders that were undecided or blacklisted and that are now checked should go on the white list.
        // The user confirmed them already just now.
        QStringList toAddToWhiteList = ((oldBlackListSet + folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncUndecidedList, &ok).toSet()) - blackListSet).toList();

        if (!toAddToWhiteList.isEmpty()) {
            auto whiteList = folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncWhiteList, &ok);
            if (ok) {
                whiteList += toAddToWhiteList;
                folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncWhiteList, whiteList);
            }
        }
        // clear the undecided list
        folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncUndecidedList, QStringList());

        // do the sync if there were changes
        auto changes = (oldBlackListSet - blackListSet) + (blackListSet - oldBlackListSet);
        if (!changes.isEmpty()) {
            if (folder->isBusy()) {
                folder->slotTerminateSync();
            }
            //The part that changed should not be read from the DB on next sync because there might be new folders
            // (the ones that are no longer in the blacklist)
            foreach (const auto &it, changes) {
                folder->journalDb()->schedulePathForRemoteDiscovery(it);
                folder->schedulePathForLocalDiscovery(it);
            }
            FolderMan::instance()->scheduleFolder(folder);
        }
    }

    resetFolders();
}

void FolderStatusModel::slotSetProgress(const ProgressInfo &progress)
{
    auto par = qobject_cast<QWidget *>(QObject::parent());
    if (!par->isVisible()) {
        return; // for https://github.com/owncloud/client/issues/2648#issuecomment-71377909
    }

    auto *f = qobject_cast<Folder *>(sender());
    if (!f) {
        return;
    }

    int folderIndex = -1;
    for (int i = 0; i < _folders.count(); ++i) {
        if (_folders.at(i)._folder == f) {
            folderIndex = i;
            break;
        }
    }
    if (folderIndex < 0) {
        return;
    }

    auto *pi = &_folders[folderIndex]._progress;

    QVector<int> roles;
    roles << FolderStatusDelegate::SyncProgressItemString
          << FolderStatusDelegate::WarningCount
          << Qt::ToolTipRole;

    if (progress.status() == ProgressInfo::Discovery) {
        if (!progress._currentDiscoveredRemoteFolder.isEmpty()) {
            pi->_overallSyncString = tr("Checking for changes in remote \"%1\"").arg(progress._currentDiscoveredRemoteFolder);
            emit dataChanged(index(folderIndex), index(folderIndex), roles);
            return;
        } else if (!progress._currentDiscoveredLocalFolder.isEmpty()) {
            pi->_overallSyncString = tr("Checking for changes in local \"%1\"").arg(progress._currentDiscoveredLocalFolder);
            emit dataChanged(index(folderIndex), index(folderIndex), roles);
            return;
        }
    }

    if (progress.status() == ProgressInfo::Reconcile) {
        pi->_overallSyncString = tr("Reconciling changes");
        emit dataChanged(index(folderIndex), index(folderIndex), roles);
        return;
    }

    // Status is Starting, Propagation or Done

    if (!progress._lastCompletedItem.isEmpty()
        && Progress::isWarningKind(progress._lastCompletedItem._status)) {
        pi->_warningCount++;
    }

    // find the single item to display:  This is going to be the bigger item, or the last completed
    // item if no items are in progress.
    SyncFileItem curItem = progress._lastCompletedItem;
    qint64 curItemProgress = -1; // -1 means finished
    qint64 biggerItemSize = 0;
    quint64 estimatedUpBw = 0;
    quint64 estimatedDownBw = 0;
    QString allFilenames;
    foreach (const ProgressInfo::ProgressItem &citm, progress._currentItems) {
        if (curItemProgress == -1 || (ProgressInfo::isSizeDependent(citm._item)
                                         && biggerItemSize < citm._item._size)) {
            curItemProgress = citm._progress.completed();
            curItem = citm._item;
            biggerItemSize = citm._item._size;
        }
        if (citm._item._direction != SyncFileItem::Up) {
            estimatedDownBw += progress.fileProgress(citm._item).estimatedBandwidth;
        } else {
            estimatedUpBw += progress.fileProgress(citm._item).estimatedBandwidth;
        }
        auto fileName = QFileInfo(citm._item._file).fileName();
        if (allFilenames.length() > 0) {
            //: Build a list of file names
            allFilenames.append(QStringLiteral(", \"%1\"").arg(fileName));
        } else {
            //: Argument is a file name
            allFilenames.append(QStringLiteral("\"%1\"").arg(fileName));
        }
    }
    if (curItemProgress == -1) {
        curItemProgress = curItem._size;
    }

    QString itemFileName = curItem._file;
    QString kindString = Progress::asActionString(curItem);

    QString fileProgressString;
    if (ProgressInfo::isSizeDependent(curItem)) {
        QString s1 = Utility::octetsToString(curItemProgress);
        QString s2 = Utility::octetsToString(curItem._size);
        //quint64 estimatedBw = progress.fileProgress(curItem).estimatedBandwidth;
        if (estimatedUpBw || estimatedDownBw) {
            /*
            //: Example text: "uploading foobar.png (1MB of 2MB) time left 2 minutes at a rate of 24Kb/s"
            fileProgressString = tr("%1 %2 (%3 of %4) %5 left at a rate of %6/s")
                .arg(kindString, itemFileName, s1, s2,
                    Utility::durationToDescriptiveString(progress.fileProgress(curItem).estimatedEta),
                    Utility::octetsToString(estimatedBw) );
            */
            //: Example text: "Syncing 'foo.txt', 'bar.txt'"
            fileProgressString = tr("Syncing %1").arg(allFilenames);
            if (estimatedDownBw > 0) {
                fileProgressString.append(tr(", "));
// ifdefs: https://github.com/owncloud/client/issues/3095#issuecomment-128409294
#ifdef Q_OS_WIN
                //: Example text: "download 24Kb/s"   (%1 is replaced by 24Kb (translated))
                fileProgressString.append(tr("download %1/s").arg(Utility::octetsToString(estimatedDownBw)));
#else
                fileProgressString.append(tr("\u2193 %1/s")
                                              .arg(Utility::octetsToString(estimatedDownBw)));
#endif
            }
            if (estimatedUpBw > 0) {
                fileProgressString.append(tr(", "));
#ifdef Q_OS_WIN
                //: Example text: "upload 24Kb/s"   (%1 is replaced by 24Kb (translated))
                fileProgressString.append(tr("upload %1/s").arg(Utility::octetsToString(estimatedUpBw)));
#else
                fileProgressString.append(tr("\u2191 %1/s")
                                              .arg(Utility::octetsToString(estimatedUpBw)));
#endif
            }
        } else {
            //: Example text: "uploading foobar.png (2MB of 2MB)"
            fileProgressString = tr("%1 %2 (%3 of %4)").arg(kindString, itemFileName, s1, s2);
        }
    } else if (!kindString.isEmpty()) {
        //: Example text: "uploading foobar.png"
        fileProgressString = tr("%1 %2").arg(kindString, itemFileName);
    }
    pi->_progressString = fileProgressString;

    // overall progress
    qint64 completedSize = progress.completedSize();
    qint64 completedFile = progress.completedFiles();
    qint64 currentFile = progress.currentFile();
    qint64 totalSize = qMax(completedSize, progress.totalSize());
    qint64 totalFileCount = qMax(currentFile, progress.totalFiles());
    QString overallSyncString;
    if (totalSize > 0) {
        QString s1 = Utility::octetsToString(completedSize);
        QString s2 = Utility::octetsToString(totalSize);

        if (progress.trustEta()) {
            //: Example text: "5 minutes left, 12 MB of 345 MB, file 6 of 7"
            overallSyncString = tr("%5 left, %1 of %2, file %3 of %4")
                                    .arg(s1, s2)
                                    .arg(currentFile)
                                    .arg(totalFileCount)
                                    .arg(Utility::durationToDescriptiveString1(progress.totalProgress().estimatedEta));

        } else {
            //: Example text: "12 MB of 345 MB, file 6 of 7"
            overallSyncString = tr("%1 of %2, file %3 of %4")
                                    .arg(s1, s2)
                                    .arg(currentFile)
                                    .arg(totalFileCount);
        }
    } else if (totalFileCount > 0) {
        // Don't attempt to estimate the time left if there is no kb to transfer.
        overallSyncString = tr("file %1 of %2").arg(currentFile).arg(totalFileCount);
    }

    pi->_overallSyncString = overallSyncString;

    int overallPercent = 0;
    if (totalFileCount > 0) {
        // Add one 'byte' for each file so the percentage is moving when deleting or renaming files
        overallPercent = qRound(double(completedSize + completedFile) / double(totalSize + totalFileCount) * 100.0);
    }
    pi->_overallPercent = qBound(0, overallPercent, 100);
    emit dataChanged(index(folderIndex), index(folderIndex), roles);
}

void FolderStatusModel::slotFolderSyncStateChange(Folder *f)
{
    if (!f) {
        return;
    }

    int folderIndex = -1;
    for (int i = 0; i < _folders.count(); ++i) {
        if (_folders.at(i)._folder == f) {
            folderIndex = i;
            break;
        }
    }
    if (folderIndex < 0) {
        return;
    }

    auto &pi = _folders[folderIndex]._progress;

    SyncResult::Status state = f->syncResult().status();
    if (!f->canSync() || state == SyncResult::Problem || state == SyncResult::Success || state == SyncResult::Error) {
        // Reset progress info.
        pi = SubFolderInfo::Progress();
    } else if (state == SyncResult::NotYetStarted) {
        FolderMan *folderMan = FolderMan::instance();
        int pos = folderMan->scheduleQueue().indexOf(f);
        for (auto other : folderMan->map()) {
            if (other != f && other->isSyncRunning())
                pos += 1;
        }
        QString message;
        if (pos <= 0) {
            message = tr("Waiting …");
        } else {
            message = tr("Waiting for %n other folder(s) …", "", pos);
        }
        pi = SubFolderInfo::Progress();
        pi._overallSyncString = message;
    } else if (state == SyncResult::SyncPrepare) {
        pi = SubFolderInfo::Progress();
        pi._overallSyncString = tr("Preparing to sync …");
    }

    // update the icon etc. now
    slotUpdateFolderState(f);

    if (f->syncResult().folderStructureWasChanged()
        && (state == SyncResult::Success || state == SyncResult::Problem)) {
        // There is a new or a removed folder. reset all data
        resetAndFetch(index(folderIndex));
    }
}

void FolderStatusModel::slotFolderScheduleQueueChanged()
{
    // Update messages on waiting folders.
    foreach (Folder *f, FolderMan::instance()->map()) {
        slotFolderSyncStateChange(f);
    }
}

void FolderStatusModel::resetFolders()
{
    setAccountState(_accountState);
}

void FolderStatusModel::slotSyncAllPendingBigFolders()
{
    for (int i = 0; i < _folders.count(); ++i) {
        if (!_folders[i]._fetched) {
            _folders[i]._folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncUndecidedList, QStringList());
            continue;
        }
        auto folder = _folders.at(i)._folder;

        bool ok = false;
        auto undecidedList = folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncUndecidedList, &ok);
        if (!ok) {
            qCWarning(lcFolderStatus) << "Could not read selective sync list from db.";
            return;
        }

        // If this folder had no undecided entries, skip it.
        if (undecidedList.isEmpty()) {
            continue;
        }

        // Remove all undecided folders from the blacklist
        auto blackList = folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, &ok);
        if (!ok) {
            qCWarning(lcFolderStatus) << "Could not read selective sync list from db.";
            return;
        }
        foreach (const auto &undecidedFolder, undecidedList) {
            blackList.removeAll(undecidedFolder);
        }
        folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, blackList);

        // Add all undecided folders to the white list
        auto whiteList = folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncWhiteList, &ok);
        if (!ok) {
            qCWarning(lcFolderStatus) << "Could not read selective sync list from db.";
            return;
        }
        whiteList += undecidedList;
        folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncWhiteList, whiteList);

        // Clear the undecided list
        folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncUndecidedList, QStringList());

        // Trigger a sync
        if (folder->isBusy()) {
            folder->slotTerminateSync();
        }
        // The part that changed should not be read from the DB on next sync because there might be new folders
        // (the ones that are no longer in the blacklist)
        foreach (const auto &it, undecidedList) {
            folder->journalDb()->schedulePathForRemoteDiscovery(it);
            folder->schedulePathForLocalDiscovery(it);
        }
        FolderMan::instance()->scheduleFolder(folder);
    }

    resetFolders();
}

void FolderStatusModel::slotSyncNoPendingBigFolders()
{
    for (int i = 0; i < _folders.count(); ++i) {
        auto folder = _folders.at(i)._folder;

        // clear the undecided list
        folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncUndecidedList, QStringList());
    }

    resetFolders();
}

void FolderStatusModel::slotNewBigFolder()
{
    auto f = qobject_cast<Folder *>(sender());
    ASSERT(f);

    int folderIndex = -1;
    for (int i = 0; i < _folders.count(); ++i) {
        if (_folders.at(i)._folder == f) {
            folderIndex = i;
            break;
        }
    }
    if (folderIndex < 0) {
        return;
    }

    resetAndFetch(index(folderIndex));

    emit suggestExpand(index(folderIndex));
    emit dirtyChanged();
}

void FolderStatusModel::slotShowFetchProgress()
{
    QMutableMapIterator<QPersistentModelIndex, QElapsedTimer> it(_fetchingItems);
    while (it.hasNext()) {
        it.next();
        if (it.value().elapsed() > 800) {
            auto idx = it.key();
            auto *info = infoForIndex(idx);
            if (info && info->_fetchingJob) {
                bool add = !info->hasLabel();
                if (add) {
                    beginInsertRows(idx, 0, 0);
                }
                info->_fetchingLabel = true;
                if (add) {
                    endInsertRows();
                }
            }
            it.remove();
        }
    }
}

bool FolderStatusModel::SubFolderInfo::hasLabel() const
{
    return _hasError || _fetchingLabel;
}

void FolderStatusModel::SubFolderInfo::resetSubs(FolderStatusModel *model, QModelIndex index)
{
    _fetched = false;
    if (_fetchingJob) {
        disconnect(_fetchingJob, nullptr, model, nullptr);
        _fetchingJob->deleteLater();
        _fetchingJob.clear();
    }
    if (hasLabel()) {
        model->beginRemoveRows(index, 0, 0);
        _fetchingLabel = false;
        _hasError = false;
        model->endRemoveRows();
    } else if (!_subs.isEmpty()) {
        model->beginRemoveRows(index, 0, _subs.count() - 1);
        _subs.clear();
        model->endRemoveRows();
    }
}


} // namespace OCC
