/*
 * Copyright (C) by Felix Weilbach <felix.weilbach@nextcloud.com>
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
#pragma once

#include <unordered_map>

#include <QObject>
#include <QAbstractItemModel>
#include <QMetaType>
#include <QIcon>
#include <QTimer>
#include <QDateTime>

#include "activityjob.h"
#include "tray/ActivityData.h"

namespace OCC {

class FileActivity
{
public:
    enum class Type {
        Unknown,
        Created,
        Shared,
        Changed,
    };

    FileActivity();

    FileActivity(int id, const QString &message, const QDateTime &timestamp, Type icon);

    int id() const;

    QString message() const;

    void setMessage(const QString &message);

    QDateTime timestamp() const;

    void setTimestamp(const QDateTime &dateTime);

    Type type() const;

    void setType(Type type);

private:
    int _id;
    QString _message;
    QDateTime _timestamp;
    Type _type;
};

class FileActivityListModel : public QAbstractListModel
{
public:
    explicit FileActivityListModel(QObject *parent = nullptr);

    void addFileActivity(const FileActivity &fileActivity);

    void addFileActivities(const std::vector<FileActivity> &fileActivities);

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;

    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;

    QVariant headerData(int section, Qt::Orientation orientation, int role = Qt::DisplayRole) const override;

    static std::shared_ptr<QPixmap> pixmapForActivityType(FileActivity::Type type, int size);

private:
    static std::shared_ptr<QPixmap> getIconPixmap(const QString &iconName, int size);

    std::unordered_map<int, std::tuple<int, FileActivity *>> _fileActivityMap;
    std::vector<std::unique_ptr<FileActivity>> _fileActivities;
    static std::unordered_map<int, std::unordered_map<QString, std::shared_ptr<QPixmap>>> iconCache;
};

class FileActivityDialogModel : public QObject
{
    Q_OBJECT

public:
    explicit FileActivityDialogModel(std::unique_ptr<ActivityJob> activityJob, PushNotifications *pushNotifications = nullptr, QObject *parent = nullptr);

    void start(const QString &fileId);

    FileActivityListModel *getActivityListModel();

    void setActivityPollInterval(int interval);

signals:
    void showActivities();
    void hideActivities();
    void showError(const QString &message);
    void hideError();
    void showProgress();
    void hideProgress();

private:
    void queryActivities();
    void activitiesReceived(const std::vector<Activity> &activites);
    void onErrorFetchingActivities();

    FileActivityListModel _fileActivityListModel;
    std::unique_ptr<ActivityJob> _activityJob;
    QTimer _activitiesPollTimer;
    QString _fileId;
};
}

Q_DECLARE_METATYPE(OCC::FileActivity)
