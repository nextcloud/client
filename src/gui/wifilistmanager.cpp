#include "wifilistmanager.h"
#include "ui_wifilistmanager.h"
#include "networksettings.h"

#include "configfile.h"

#include <QPushButton>

namespace OCC {

WifiListManager::WifiListManager(QWidget *parent)
    : QDialog(parent)
    , _ui(new Ui::WifiListManager)
{
    _ui->setupUi(this);

    this->setAttribute(Qt::WA_DeleteOnClose);

    loadListMode();
    loadSsidList();

    connect(_ui->saveExitBtn, &QPushButton::clicked, this, &WifiListManager::saveSsidSettings);
    connect(_ui->cancelExitBtn, &QPushButton::clicked, [this]() { this->close(); });
    connect(_ui->addToListBtn, &QPushButton::clicked, [this]() { WifiListManager::addListItem(_ui->ssidEntryEdit->text()); });
    connect(_ui->deleteFromListBtn, &QPushButton::clicked, [this]() { WifiListManager::deleteListItems(_ui->ssidList->selectedItems()); });
    connect(_ui->ssidEntryEdit, &QLineEdit::editingFinished, [this]() { WifiListManager::addListItem(_ui->ssidEntryEdit->text()); _ui->ssidEntryEdit->setText(QString("")); });
}

WifiListManager::~WifiListManager()
{
    delete _ui;
}

void WifiListManager::loadListMode()
{
    OCC::ConfigFile cfg;
    auto test = cfg.wifiListMode();
    (cfg.wifiListMode() == "whitelist") ? _ui->whitelistCheckbox->setChecked(true) : _ui->blacklistCheckbox->setChecked(true);
}

void WifiListManager::loadSsidList()
{
    OCC::ConfigFile cfg;
    QStringList ssidLoadedList = cfg.ssidList();
    if ( !(ssidLoadedList.count() == 1 && ssidLoadedList.at(0).isEmpty()) ) {
            _ui->ssidList->addItems(ssidLoadedList);
    }
}

void WifiListManager::saveSsidSettings()
{
    OCC::ConfigFile cfg;
    cfg.setWifiListMode((_ui->whitelistCheckbox->isChecked()) ? QString("whitelist") : QString("blacklist"));

    QStringList *ssidSaveList;
    ssidSaveList = new QStringList;
    if (_ui->ssidList->count() != 0) {
        for (int i = 0; i < _ui->ssidList->count(); i++) {
            ssidSaveList->append(_ui->ssidList->item(i)->text());
        }
    } 
    cfg.setSsidList(ssidSaveList);
    delete ssidSaveList;
    this->close();
}

void WifiListManager::addListItem(const QString &entry)
{
    if (entry.isEmpty()) {
        // TODO: Feedback
        return;
    }
    _ui->ssidList->addItem(entry);
    ;
    return;
}

void WifiListManager::deleteListItems(const QList<QListWidgetItem *> &itemList)
{
    if (!itemList.isEmpty()) {
        for (int i = 0; i < itemList.count(); i++) {
            delete _ui->ssidList->takeItem(_ui->ssidList->row(itemList[i]));
        }
        return;
    }
}

} // namespace OCC
