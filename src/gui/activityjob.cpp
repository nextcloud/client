#include <QUrlQuery>
#include <QJsonArray>

#include "activityjob.h"
#include "iconjob.h"
#include "tray/ActivityData.h"

Q_LOGGING_CATEGORY(lcActivityJob, "nextcloud.gui.activityjob", QtInfoMsg)

namespace OCC {

ActivityJob::ActivityJob(QObject *parent)
    : QObject(parent)
{
}

OcsActivityJob::OcsActivityJob(AccountPtr account, QObject *parent)
    : ActivityJob(parent)
    , _account(account)
{
}

void OcsActivityJob::queryActivities(const Optional<QString> &objectType, const Optional<QString> &objectId, int limit)
{
    _limit = limit;
    _objectId = objectId;
    _objectType = objectType;

    startJsonApiJob();
}

void OcsActivityJob::startJsonApiJob(const Optional<QString> since)
{
    const auto url = _objectType || _objectId ? QStringLiteral("ocs/v2.php/apps/activity/api/v2/activity/filter")
                                              : QStringLiteral("ocs/v2.php/apps/activity/api/v2/activity");
    auto job = new JsonApiJob(_account, url, this);
    QObject::connect(job, &JsonApiJob::jsonReceived,
        this, [this, job](const QJsonDocument &json, int statusCode) {
            jsonApiJobFinished(*job, json, statusCode);
        });

    QUrlQuery params;
    if (_objectType) {
        params.addQueryItem(QStringLiteral("object_type"), *_objectType);
    }
    if (_objectId) {
        params.addQueryItem(QStringLiteral("object_id"), *_objectId);
    }
    if (since) {
        params.addQueryItem(QStringLiteral("since"), *since);
    }
    params.addQueryItem(QStringLiteral("limit"), QString::number(_limit));
    job->addQueryParams(params);
    job->start();
}

Activity OcsActivityJob::jsonObjectToActivity(QJsonObject activityJson)
{
    Activity activity;
    activity._type = Activity::ActivityType;
    activity._objectType = activityJson.value("object_type").toString();
    activity._accName = _account->displayName();
    activity._id = activityJson.value("activity_id").toInt();
    activity._fileAction = activityJson.value("type").toString();
    activity._subject = activityJson.value("subject").toString();
    activity._message = activityJson.value("message").toString();
    activity._file = activityJson.value("object_name").toString();
    activity._link = QUrl(activityJson.value("link").toString());
    activity._dateTime = QDateTime::fromString(activityJson.value("datetime").toString(), Qt::ISODate);
    activity._icon = activityJson.value("icon").toString();

    return activity;
}

std::vector<Activity> OcsActivityJob::jsonArrayToActivities(QJsonArray activitiesJson)
{
    std::vector<Activity> activities;
    for (const auto activ : activitiesJson) {
        activities.emplace_back(jsonObjectToActivity(activ.toObject()));
    }
    return activities;
}

void OcsActivityJob::processActivities(const QJsonDocument &json)
{
    const auto activitiesJson = json.object().value(QStringLiteral("ocs")).toObject().value(QStringLiteral("data")).toArray();
    emit finished(jsonArrayToActivities(activitiesJson));
}

QUrl OcsActivityJob::parseLinkFromHeader(const QByteArray &linkRawHeader)
{
    // The link is ecapsulated in a tag. We need to parse this tag.

    const auto linkHeaderTagMinimumExpectedLength = 14;
    if (linkRawHeader.size() <= linkHeaderTagMinimumExpectedLength) {
        qCWarning(lcActivityJob) << "Link to next page submitted, but could not be parsed";
        return {};
    }

    const auto linkHeaderTagUnnecessaryExcessPrefixLength = 1;
    const auto linkHeaderTagUnnecessaryExcessSuffixLength = 14;
    return QString(linkRawHeader.chopped(linkHeaderTagUnnecessaryExcessSuffixLength)
                       .mid(linkHeaderTagUnnecessaryExcessPrefixLength));
}

QString OcsActivityJob::getQueryItemFromLink(const QUrl &url, const QString &queryItemName)
{
    QUrlQuery query(url);
    if (!query.hasQueryItem(queryItemName)) {
        return {};
    }
    return query.queryItemValue(queryItemName);
}

QString OcsActivityJob::getSinceQueryItemFromLink(const QUrl &url)
{
    return getQueryItemFromLink(url, QStringLiteral("since"));
}

void OcsActivityJob::processNextPage(const QNetworkReply *reply)
{
    if (reply->hasRawHeader("Link")) {
        const auto nextLink = parseLinkFromHeader(reply->rawHeader("Link"));
        if (!nextLink.isValid()) {
            qCWarning(lcActivityJob) << "Link to next page submitted, but could not be parsed";
            emit error();
        }
        const auto since = getSinceQueryItemFromLink(nextLink);
        if (since.isEmpty()) {
            qCWarning(lcActivityJob) << "Link to next page submitted, but the link has no since query item";
            emit error();
        }
        startJsonApiJob(since);
    }
}

void OcsActivityJob::jsonApiJobFinished(const JsonApiJob &job, const QJsonDocument &json, int statusCode)
{
    if (statusCode != 200) {
        emit error();
        return;
    }

    processActivities(json);
    processNextPage(job.reply());
}
}
