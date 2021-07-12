#include "ignorelisttablewidget.h"
#include "ui_ignorelisttablewidget.h"

#include "folderman.h"

#include <QFile>
#include <QInputDialog>
#include <QLineEdit>
#include <QMessageBox>

namespace OCC {

static constexpr int patternCol = 0;
static constexpr int deletableCol = 1;
static constexpr int readOnlyRows = 3;

IgnoreListTableWidget::IgnoreListTableWidget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::IgnoreListTableWidget)
{
    setWindowFlags(windowFlags() & ~Qt::WindowContextHelpButtonHint);
    ui->setupUi(this);

    ui->descriptionLabel->setText(tr("Files or folders matching a pattern will not be synchronized.\n\n"
                                     "Items where deletion is allowed will be deleted if they prevent a "
                                     "directory from being removed. "
                                     "This is useful for meta data."));

    ui->removePushButton->setEnabled(false);
    connect(ui->tableWidget,         &QTableWidget::itemSelectionChanged,
            this, &IgnoreListTableWidget::slotItemSelectionChanged);
    connect(ui->removePushButton,    &QAbstractButton::clicked,
            this, &IgnoreListTableWidget::slotRemoveCurrentItem);
    connect(ui->addPushButton,       &QAbstractButton::clicked,
            this, &IgnoreListTableWidget::slotAddPattern);
    connect(ui->removeAllPushButton, &QAbstractButton::clicked,
            this, &IgnoreListTableWidget::slotRemoveAllItems);

    ui->tableWidget->resizeColumnsToContents();
    ui->tableWidget->horizontalHeader()->setSectionResizeMode(patternCol, QHeaderView::Stretch);
    ui->tableWidget->verticalHeader()->setVisible(false);
}

IgnoreListTableWidget::~IgnoreListTableWidget()
{
    delete ui;
}

void IgnoreListTableWidget::slotItemSelectionChanged()
{
    QTableWidgetItem *item = ui->tableWidget->currentItem();
    if (!item) {
        ui->removePushButton->setEnabled(false);
        return;
    }

    bool enable = item->flags() & Qt::ItemIsEnabled;
    ui->removePushButton->setEnabled(enable);
}

void IgnoreListTableWidget::slotRemoveCurrentItem()
{
    ui->tableWidget->removeRow(ui->tableWidget->currentRow());
    if(ui->tableWidget->rowCount() == readOnlyRows)
        ui->removeAllPushButton->setEnabled(false);
}

void IgnoreListTableWidget::slotRemoveAllItems()
{
    ui->tableWidget->setRowCount(0);
}

void IgnoreListTableWidget::slotWriteIgnoreFile(const QString & file)
{
    QFile ignores(file);
    if (ignores.open(QIODevice::WriteOnly)) {
        // rewrites the whole file since now the user can also remove system patterns
        QFile::resize(file, 0);
        for (int row = 0; row < ui->tableWidget->rowCount(); ++row) {
            QTableWidgetItem *patternItem = ui->tableWidget->item(row, patternCol);
            QTableWidgetItem *deletableItem = ui->tableWidget->item(row, deletableCol);
            if (patternItem->flags() & Qt::ItemIsEnabled) {
                QByteArray prepend;
                if (deletableItem->checkState() == Qt::Checked) {
                    prepend = "]";
                } else if (patternItem->text().startsWith('#')) {
                    prepend = "\\";
                }
                ignores.write(prepend + patternItem->text().toUtf8() + '\n');
            }
        }
    } else {
        QMessageBox::warning(this, tr("Could not open file"),
            tr("Cannot write changes to \"%1\".").arg(file));
    }
    ignores.close(); //close the file before reloading stuff.

    FolderMan *folderMan = FolderMan::instance();

    // We need to force a remote discovery after a change of the ignore list.
    // Otherwise we would not download the files/directories that are no longer
    // ignored (because the remote etag did not change)   (issue #3172)
    foreach (Folder *folder, folderMan->map()) {
        folder->journalDb()->forceRemoteDiscoveryNextSync();
        folderMan->scheduleFolder(folder);
    }
}

void IgnoreListTableWidget::slotAddPattern()
{
    bool okClicked = false;
    QString pattern = QInputDialog::getText(this, tr("Add Ignore Pattern"),
        tr("Add a new ignore pattern:"),
        QLineEdit::Normal, QString(), &okClicked);

    if (!okClicked || pattern.isEmpty())
        return;

    addPattern(pattern, false, false);
    ui->tableWidget->scrollToBottom();
}

void IgnoreListTableWidget::readIgnoreFile(const QString &file, bool readOnly)
{
    QFile ignores(file);
    if (ignores.open(QIODevice::ReadOnly)) {
        while (!ignores.atEnd()) {
            QString line = QString::fromUtf8(ignores.readLine());
            line.chop(1);
            if (!line.isEmpty() && !line.startsWith("#")) {
                bool deletable = false;
                if (line.startsWith(']')) {
                    deletable = true;
                    line = line.mid(1);
                }
                addPattern(line, deletable, readOnly);
            }
        }
    }
}

int IgnoreListTableWidget::addPattern(const QString &pattern, bool deletable, bool readOnly)
{
    int newRow = ui->tableWidget->rowCount();
    ui->tableWidget->setRowCount(newRow + 1);

    auto *patternItem = new QTableWidgetItem;
    patternItem->setText(pattern);
    ui->tableWidget->setItem(newRow, patternCol, patternItem);

    auto *deletableItem = new QTableWidgetItem;
    deletableItem->setFlags(Qt::ItemIsUserCheckable | Qt::ItemIsEnabled);
    deletableItem->setCheckState(deletable ? Qt::Checked : Qt::Unchecked);
    ui->tableWidget->setItem(newRow, deletableCol, deletableItem);

    if (readOnly) {
        patternItem->setFlags(patternItem->flags() ^ Qt::ItemIsEnabled);
        patternItem->setToolTip(readOnlyTooltip);
        deletableItem->setFlags(deletableItem->flags() ^ Qt::ItemIsEnabled);
    }

    ui->removeAllPushButton->setEnabled(true);

    return newRow;
}

} // namespace OCC
