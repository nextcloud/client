/*
 * Copyright (C) by Olivier Goffart <ogoffart@woboq.com>
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
#include "selectivesyncdialog.h"
#include "folder.h"
#include "account.h"
#include "networkjobs.h"
#include "theme.h"
#include "folderman.h"
#include "configfile.h"
#include <QDialogButtonBox>
#include <QVBoxLayout>
#include <QTreeWidget>
#include <qpushbutton.h>
#include <QFileIconProvider>
#include <QHeaderView>
#include <QSettings>
#include <QScopedValueRollback>
#include <QTreeWidgetItem>
#include <QLabel>
#include <QVBoxLayout>

namespace OCC {


class SelectiveSyncTreeViewItem : public QTreeWidgetItem
{
public:
    SelectiveSyncTreeViewItem(int type = QTreeWidgetItem::Type)
        : QTreeWidgetItem(type)
    {
    }
    SelectiveSyncTreeViewItem(const QStringList &strings, int type = QTreeWidgetItem::Type)
        : QTreeWidgetItem(strings, type)
    {
    }
    SelectiveSyncTreeViewItem(QTreeWidget *view, int type = QTreeWidgetItem::Type)
        : QTreeWidgetItem(view, type)
    {
    }
    SelectiveSyncTreeViewItem(QTreeWidgetItem *parent, int type = QTreeWidgetItem::Type)
        : QTreeWidgetItem(parent, type)
    {
    }

private:
    bool operator<(const QTreeWidgetItem &other) const override
    {
        int column = treeWidget()->sortColumn();
        if (column == 1) {
            return data(1, Qt::UserRole).toLongLong() < other.data(1, Qt::UserRole).toLongLong();
        }
        return QTreeWidgetItem::operator<(other);
    }
};

SelectiveSyncWidget::SelectiveSyncWidget(AccountPtr account, QWidget *parent)
    : QWidget(parent)
    , _account(account)
    , _inserting(false)
    , _folderTree(new QTreeWidget(this))
{
    _loading = new QLabel(tr("Loading …"), _folderTree);

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto header = new QLabel(this);
    header->setText(tr("Deselect remote folders you do not wish to synchronize."));
    header->setWordWrap(true);
    layout->addWidget(header);

    layout->addWidget(_folderTree);

    connect(_folderTree, &QTreeWidget::itemExpanded,
        this, &SelectiveSyncWidget::slotItemExpanded);
    connect(_folderTree, &QTreeWidget::itemChanged,
        this, &SelectiveSyncWidget::slotItemChanged);
    _folderTree->setSortingEnabled(true);
    _folderTree->sortByColumn(0, Qt::AscendingOrder);
    _folderTree->setColumnCount(2);
    _folderTree->header()->setSectionResizeMode(0, QHeaderView::QHeaderView::ResizeToContents);
    _folderTree->header()->setSectionResizeMode(1, QHeaderView::QHeaderView::ResizeToContents);
    _folderTree->header()->setStretchLastSection(true);
    _folderTree->headerItem()->setText(0, tr("Name"));
    _folderTree->headerItem()->setText(1, tr("Size"));

    ConfigFile::setupDefaultExcludeFilePaths(_excludedFiles);
    _excludedFiles.reloadExcludeFiles();
}

QSize SelectiveSyncWidget::sizeHint() const
{
    return QWidget::sizeHint().expandedTo(QSize(600, 600));
}

void SelectiveSyncWidget::refreshFolders()
{
    LsColJob *job = new LsColJob(_account, _folderPath, this);
    job->setProperties(QList<QByteArray>() << "resourcetype"
                                           << "http://owncloud.org/ns:size");
    connect(job, &LsColJob::directoryListingSubfolders,
        this, &SelectiveSyncWidget::slotUpdateDirectories);
    connect(job, &LsColJob::finishedWithError,
        this, &SelectiveSyncWidget::slotLscolFinishedWithError);
    job->start();
    _folderTree->clear();
    _loading->show();
    _loading->move(10, _folderTree->header()->height() + 10);
}

void SelectiveSyncWidget::setFolderInfo(const QString &folderPath, const QString &rootName, const QStringList &oldBlackList)
{
    _folderPath = folderPath;
    if (_folderPath.startsWith(QLatin1Char('/'))) {
        // remove leading '/'
        _folderPath = folderPath.mid(1);
    }
    _rootName = rootName;
    _oldBlackList = oldBlackList;
    refreshFolders();
}

static QTreeWidgetItem *findFirstChild(QTreeWidgetItem *parent, const QString &text)
{
    for (int i = 0; i < parent->childCount(); ++i) {
        QTreeWidgetItem *child = parent->child(i);
        if (child->text(0) == text) {
            return child;
        }
    }
    return nullptr;
}

void SelectiveSyncWidget::recursiveInsert(QTreeWidgetItem *parent, QStringList pathTrail, QString path, qint64 size)
{
    QFileIconProvider prov;
    QIcon folderIcon = prov.icon(QFileIconProvider::Folder);
    if (pathTrail.size() == 0) {
        if (path.endsWith('/')) {
            path.chop(1);
        }
        parent->setToolTip(0, path);
        parent->setData(0, Qt::UserRole, path);
    } else {
        SelectiveSyncTreeViewItem *item = static_cast<SelectiveSyncTreeViewItem *>(findFirstChild(parent, pathTrail.first()));
        if (!item) {
            item = new SelectiveSyncTreeViewItem(parent);
            if (parent->checkState(0) == Qt::Checked
                || parent->checkState(0) == Qt::PartiallyChecked) {
                item->setCheckState(0, Qt::Checked);
                foreach (const QString &str, _oldBlackList) {
                    if (str == path || str == QLatin1String("/")) {
                        item->setCheckState(0, Qt::Unchecked);
                        break;
                    } else if (str.startsWith(path)) {
                        item->setCheckState(0, Qt::PartiallyChecked);
                    }
                }
            } else if (parent->checkState(0) == Qt::Unchecked) {
                item->setCheckState(0, Qt::Unchecked);
            }
            item->setIcon(0, folderIcon);
            item->setText(0, pathTrail.first());
            if (size >= 0) {
                item->setText(1, Utility::octetsToString(size));
                item->setData(1, Qt::UserRole, size);
            }
            //            item->setData(0, Qt::UserRole, pathTrail.first());
            item->setChildIndicatorPolicy(QTreeWidgetItem::ShowIndicator);
        }

        pathTrail.removeFirst();
        recursiveInsert(item, pathTrail, path, size);
    }
}

void SelectiveSyncWidget::slotUpdateDirectories(SyncObjectList list)
{
    auto job = qobject_cast<LsColJob *>(sender());
    QScopedValueRollback<bool> isInserting(_inserting);
    _inserting = true;

    SelectiveSyncTreeViewItem *root = static_cast<SelectiveSyncTreeViewItem *>(_folderTree->topLevelItem(0));

    QUrl url = _account->davUrl();
    QString pathToRemove = url.path();
    if (!pathToRemove.endsWith('/')) {
        pathToRemove.append('/');
    }
    pathToRemove.append(_folderPath);
    if (!_folderPath.isEmpty())
        pathToRemove.append('/');

    // Check for excludes.
    QMutableListIterator<QString> it(list);
    while (it.hasNext()) {
        it.next();
        if (_excludedFiles.isExcluded(it.value(), pathToRemove, FolderMan::instance()->ignoreHiddenFiles()))
            it.remove();
    }

    // Since / cannot be in the blacklist, expand it to the actual
    // list of top-level folders as soon as possible.
    if (_oldBlackList == QStringList("/")) {
        _oldBlackList.clear();
        foreach (QString path, list) {
            path.remove(pathToRemove);
            if (path.isEmpty()) {
                continue;
            }
            _oldBlackList.append(path);
        }
    }

    if (!root && list.size() <= 1) {
        _loading->setText(tr("No subfolders currently on the server."));
        _loading->resize(_loading->sizeHint()); // because it's not in a layout
        return;
    } else {
        _loading->hide();
    }

    if (!root) {
        root = new SelectiveSyncTreeViewItem(_folderTree);
        root->setText(0, _rootName);
        root->setIcon(0, Theme::instance()->applicationIcon());
        root->setData(0, Qt::UserRole, QString());
        root->setCheckState(0, Qt::Checked);
        qint64 size = job ? job->_folderInfos[pathToRemove].size : -1;
        if (size >= 0) {
            root->setText(1, Utility::octetsToString(size));
            root->setData(1, Qt::UserRole, size);
        }
    }

    Utility::sortFilenames(list);
    foreach (QString path, list) {
        auto size = job ? job->_folderInfos[path].size : 0;
        path.remove(pathToRemove);
        QStringList paths = path.split('/');
        if (paths.last().isEmpty())
            paths.removeLast();
        if (paths.isEmpty())
            continue;
        if (!path.endsWith('/')) {
            path.append('/');
        }
        recursiveInsert(root, paths, path, size);
    }

    // Root is partially checked if any children are not checked
    for (int i = 0; i < root->childCount(); ++i) {
        const auto child = root->child(i);
        if (child->checkState(0) != Qt::Checked) {
            root->setCheckState(0, Qt::PartiallyChecked);
            break;
        }
    }

    root->setExpanded(true);
}

void SelectiveSyncWidget::slotLscolFinishedWithError(QNetworkReply *r)
{
    if (r->error() == QNetworkReply::ContentNotFoundError) {
        _loading->setText(tr("No subfolders currently on the server."));
    } else {
        _loading->setText(tr("An error occurred while loading the list of sub folders."));
    }
    _loading->resize(_loading->sizeHint()); // because it's not in a layout
}

void SelectiveSyncWidget::slotItemExpanded(QTreeWidgetItem *item)
{
    QString dir = item->data(0, Qt::UserRole).toString();
    if (dir.isEmpty())
        return;
    QString prefix;
    if (!_folderPath.isEmpty()) {
        prefix = _folderPath + QLatin1Char('/');
    }
    LsColJob *job = new LsColJob(_account, prefix + dir, this);
    job->setProperties(QList<QByteArray>() << "resourcetype"
                                           << "http://owncloud.org/ns:size");
    connect(job, &LsColJob::directoryListingSubfolders,
        this, &SelectiveSyncWidget::slotUpdateDirectories);
    job->start();
}

void SelectiveSyncWidget::slotItemChanged(QTreeWidgetItem *item, int col)
{
    if (col != 0 || _inserting)
        return;

    if (item->checkState(0) == Qt::Checked) {
        // If we are checked, check that we may need to check the parent as well if
        // all the siblings are also checked
        QTreeWidgetItem *parent = item->parent();
        if (parent && parent->checkState(0) != Qt::Checked) {
            bool hasUnchecked = false;
            for (int i = 0; i < parent->childCount(); ++i) {
                if (parent->child(i)->checkState(0) != Qt::Checked) {
                    hasUnchecked = true;
                    break;
                }
            }
            if (!hasUnchecked) {
                parent->setCheckState(0, Qt::Checked);
            } else if (parent->checkState(0) == Qt::Unchecked) {
                parent->setCheckState(0, Qt::PartiallyChecked);
            }
        }
        // also check all the children
        for (int i = 0; i < item->childCount(); ++i) {
            if (item->child(i)->checkState(0) != Qt::Checked) {
                item->child(i)->setCheckState(0, Qt::Checked);
            }
        }
    }

    if (item->checkState(0) == Qt::Unchecked) {
        QTreeWidgetItem *parent = item->parent();
        if (parent && parent->checkState(0) == Qt::Checked) {
            parent->setCheckState(0, Qt::PartiallyChecked);
        }

        // Uncheck all the children
        for (int i = 0; i < item->childCount(); ++i) {
            if (item->child(i)->checkState(0) != Qt::Unchecked) {
                item->child(i)->setCheckState(0, Qt::Unchecked);
            }
        }

        // Can't uncheck the root.
        if (!parent) {
            item->setCheckState(0, Qt::PartiallyChecked);
        }
    }

    if (item->checkState(0) == Qt::PartiallyChecked) {
        QTreeWidgetItem *parent = item->parent();
        if (parent && parent->checkState(0) != Qt::PartiallyChecked) {
            parent->setCheckState(0, Qt::PartiallyChecked);
        }
    }
}

QStringList SelectiveSyncWidget::createBlackList(QTreeWidgetItem *root) const
{
    if (!root) {
        root = _folderTree->topLevelItem(0);
    }
    if (!root)
        return QStringList();

    switch (root->checkState(0)) {
    case Qt::Unchecked:
        return QStringList(root->data(0, Qt::UserRole).toString() + "/");
    case Qt::Checked:
        return QStringList();
    case Qt::PartiallyChecked:
        break;
    }

    QStringList result;
    if (root->childCount()) {
        for (int i = 0; i < root->childCount(); ++i) {
            result += createBlackList(root->child(i));
        }
    } else {
        // We did not load from the server so we re-use the one from the old black list
        QString path = root->data(0, Qt::UserRole).toString();
        foreach (const QString &it, _oldBlackList) {
            if (it.startsWith(path))
                result += it;
        }
    }
    return result;
}

QStringList SelectiveSyncWidget::oldBlackList() const
{
    return _oldBlackList;
}

qint64 SelectiveSyncWidget::estimatedSize(QTreeWidgetItem *root)
{
    if (!root) {
        root = _folderTree->topLevelItem(0);
    }
    if (!root)
        return -1;


    switch (root->checkState(0)) {
    case Qt::Unchecked:
        return 0;
    case Qt::Checked:
        return root->data(1, Qt::UserRole).toLongLong();
    case Qt::PartiallyChecked:
        break;
    }

    qint64 result = 0;
    if (root->childCount()) {
        for (int i = 0; i < root->childCount(); ++i) {
            auto r = estimatedSize(root->child(i));
            if (r < 0)
                return r;
            result += r;
        }
    } else {
        // We did not load from the server so we have no idea how much we will sync from this branch
        return -1;
    }
    return result;
}


SelectiveSyncDialog::SelectiveSyncDialog(AccountPtr account, Folder *folder, QWidget *parent, Qt::WindowFlags f)
    : QDialog(parent, f)
    , _folder(folder)
    , _okButton(nullptr) // defined in init()
{
    bool ok;
    init(account);
    QStringList selectiveSyncList = _folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, &ok);
    if (ok) {
        _selectiveSync->setFolderInfo(_folder->remotePath(), _folder->alias(), selectiveSyncList);
    } else {
        _okButton->setEnabled(false);
    }
    // Make sure we don't get crashes if the folder is destroyed while we are still open
    connect(_folder, &QObject::destroyed, this, &QObject::deleteLater);
}

SelectiveSyncDialog::SelectiveSyncDialog(AccountPtr account, const QString &folder,
    const QStringList &blacklist, QWidget *parent, Qt::WindowFlags f)
    : QDialog(parent, f)
    , _folder(nullptr)
{
    init(account);
    _selectiveSync->setFolderInfo(folder, folder, blacklist);
}

void SelectiveSyncDialog::init(const AccountPtr &account)
{
    setWindowTitle(tr("Choose What to Sync"));
    QVBoxLayout *layout = new QVBoxLayout(this);
    _selectiveSync = new SelectiveSyncWidget(account, this);
    layout->addWidget(_selectiveSync);
    QDialogButtonBox *buttonBox = new QDialogButtonBox(Qt::Horizontal);
    _okButton = buttonBox->addButton(QDialogButtonBox::Ok);
    connect(_okButton, &QPushButton::clicked, this, &SelectiveSyncDialog::accept);
    QPushButton *button;
    button = buttonBox->addButton(QDialogButtonBox::Cancel);
    connect(button, &QAbstractButton::clicked, this, &QDialog::reject);
    layout->addWidget(buttonBox);
}

void SelectiveSyncDialog::accept()
{
    if (_folder) {
        bool ok;
        auto oldBlackListSet = _folder->journalDb()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, &ok).toSet();
        if (!ok) {
            return;
        }
        QStringList blackList = _selectiveSync->createBlackList();
        _folder->journalDb()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, blackList);

        FolderMan *folderMan = FolderMan::instance();
        if (_folder->isBusy()) {
            _folder->slotTerminateSync();
        }

        //The part that changed should not be read from the DB on next sync because there might be new folders
        // (the ones that are no longer in the blacklist)
        auto blackListSet = blackList.toSet();
        auto changes = (oldBlackListSet - blackListSet) + (blackListSet - oldBlackListSet);
        foreach (const auto &it, changes) {
            _folder->journalDb()->avoidReadFromDbOnNextSync(it);
        }

        folderMan->scheduleFolder(_folder);
    }
    QDialog::accept();
}

QStringList SelectiveSyncDialog::createBlackList() const
{
    return _selectiveSync->createBlackList();
}

QStringList SelectiveSyncDialog::oldBlackList() const
{
    return _selectiveSync->oldBlackList();
}

qint64 SelectiveSyncDialog::estimatedSize()
{
    return _selectiveSync->estimatedSize();
}
}
