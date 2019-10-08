#ifndef WIFILISTMANAGER_H
#define WIFILISTMANAGER_H

#include <QDialog>
#include <QListWidgetItem>

namespace OCC {

namespace Ui {
    class WifiListManager;
}

class WifiListManager : public QDialog
{
    Q_OBJECT

public:
    explicit WifiListManager(QWidget *parent = nullptr);
    ~WifiListManager();

private slots:
    void loadListMode();
    void loadSsidList();
    void addListItem(const QString &entry);
    void deleteListItems(const QList<QListWidgetItem*> &itemList);
    void saveSsidSettings();

private:
    Ui::WifiListManager *_ui;
};
}
#endif // WIFILISTMANAGER_H
