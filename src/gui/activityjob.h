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

    virtual void queryActivities(const Optional<QString> &objectType,
        const Optional<QString> &objectId, int limit = defaultNumberActivitiesFetchedPerRequest) = 0;

    static constexpr auto defaultNumberActivitiesFetchedPerRequest = 50;

signals:
    void finished(const std::vector<Activity> &activities);

    void error();
};

class OcsActivityJob : public ActivityJob
{
public:
    explicit OcsActivityJob(AccountPtr account, QObject *parent = nullptr);

    void queryActivities(const Optional<QString> &objectType,
        const Optional<QString> &objectId, int limit = ActivityJob::defaultNumberActivitiesFetchedPerRequest) override;

private:
    void startJsonApiJob(const Optional<QString> &since = {});
    void jsonApiJobFinished(const JsonApiJob &job, const QJsonDocument &json, int statusCode);
    Activity jsonObjectToActivity(const QJsonObject &activityJson);
    std::vector<Activity> jsonArrayToActivities(const QJsonArray &activitiesJson);
    void processActivities(const QJsonDocument &json);
    void activitiesPartReceived(const QJsonDocument &json, int statusCode, const QUrl &nextLink);
    void processNextPage(const QNetworkReply *reply);

    AccountPtr _account;
    int _limit = ActivityJob::defaultNumberActivitiesFetchedPerRequest;
    Optional<QString> _objectId;
    Optional<QString> _objectType;
};
}
