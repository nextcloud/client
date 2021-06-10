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

#include <QObject>
#include <vector>

#include "common/result.h"
#include "tray/ActivityData.h"
#include "account.h"

namespace OCC {

class ActivityJob : public QObject
{
    Q_OBJECT

public:
    explicit ActivityJob(QObject *parent = nullptr);

    virtual void queryActivities(Optional<QString> objectType,
        Optional<QString> objectId, Optional<int> since = {}) = 0;

signals:
    void finished(const std::vector<Activity> &activities);

    void error();
};

class OcsActivityJob : public ActivityJob
{
public:
    explicit OcsActivityJob(AccountPtr account, QObject *parent = nullptr);

    void queryActivities(Optional<QString> objectType,
        Optional<QString> objectId, Optional<int> since = {}) override;

private:
    void startJsonApiJob(const Optional<QString> &objectType = {}, const Optional<QString> &objectId = {},
        const Optional<int> &since = {});
    void startNextJsonApiJob(const QUrl &nextLink);
    void jsonApiJobFinished(const JsonApiJob &job, const QJsonDocument &json, int statusCode);
    Activity jsonObjectToActivity(const QJsonObject &activityJson);
    std::vector<Activity> jsonArrayToActivities(const QJsonArray &activitiesJson);
    void processActivities(const QJsonDocument &json);
    void activitiesPartReceived(const QJsonDocument &json, int statusCode, const QUrl &nextLink);
    void processNextPage(const QNetworkReply *reply);

    AccountPtr _account;
    // Optional<QString> _objectId;
    // Optional<QString> _objectType;
};
}
