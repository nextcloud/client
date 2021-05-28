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

void OcsActivityJob::queryActivities(Optional<QString> objectType, Optional<QString> objectId, Optional<int> limit)
{
    startJsonApiJob(objectType, objectId, limit);
}

void OcsActivityJob::startJsonApiJob(const Optional<QString> &objectType, const Optional<QString> &objectId,
    const Optional<int> &since, const Optional<int> &limit)
{
    qCDebug(lcActivityJob) << "start activity job";
    const auto url = objectType || objectId ? QStringLiteral("ocs/v2.php/apps/activity/api/v2/activity/filter")
                                            : QStringLiteral("ocs/v2.php/apps/activity/api/v2/activity");
    auto job = new JsonApiJob(_account, url, this);
    QObject::connect(job, &JsonApiJob::jsonReceived,
        this, [this, job](const QJsonDocument &json, int statusCode) {
            jsonApiJobFinished(*job, json, statusCode);
        });

    QUrlQuery params;
    if (objectType) {
        params.addQueryItem(QStringLiteral("object_type"), *objectType);
    }
    if (objectId) {
        params.addQueryItem(QStringLiteral("object_id"), *objectId);
    }
    if (since) {
        params.addQueryItem(QStringLiteral("since"), QString::number(*since));
    }
    if (limit) {
        params.addQueryItem(QStringLiteral("limit"), QString::number(*limit));
    }
    job->addQueryParams(params);
    job->start();
}

void OcsActivityJob::startNextJsonApiJob(const QUrl &nextLink)
{
    qCDebug(lcActivityJob) << "start activity job" << nextLink;
    QString nextLinkPath = nextLink.toString();
    nextLinkPath.replace(_account->url().toString(), "");
    qCDebug(lcActivityJob) << "path" << nextLinkPath;

    auto job = new JsonApiJob(_account, nextLinkPath, this);
    QObject::connect(job, &JsonApiJob::jsonReceived,
        this, [this, job](const QJsonDocument &json, int statusCode) {
            jsonApiJobFinished(*job, json, statusCode);
        });
    job->start();
}

Activity OcsActivityJob::jsonObjectToActivity(const QJsonObject &activityJson)
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

std::vector<Activity> OcsActivityJob::jsonArrayToActivities(const QJsonArray &activitiesJson)
{
    std::vector<Activity> activities;
    for (const auto activ : activitiesJson) {
        activities.push_back(jsonObjectToActivity(activ.toObject()));
    }
    return activities;
}

void OcsActivityJob::processActivities(const QJsonDocument &json)
{
    const auto activitiesJson = json.object().value(QStringLiteral("ocs")).toObject().value(QStringLiteral("data")).toArray();
    emit finished(jsonArrayToActivities(activitiesJson));
}

static QUrl parseLinkFromHeader(const QByteArray &linkRawHeader)
{
    // The link is ecapsulated in a tag. We need to parse this tag.
    const QString tagPrefix = QStringLiteral("<");
    const QString tagSuffix = QStringLiteral(">; rel=\"next\"");

    const auto linkHeaderTagMinimumExpectedLength = tagPrefix.size() + tagSuffix.size();

    if (linkRawHeader.size() <= linkHeaderTagMinimumExpectedLength) {
        qCWarning(lcActivityJob) << "Link to next page submitted, but could not be parsed";
        return {};
    }

    auto link = linkRawHeader;
    link.replace(tagPrefix, "");
    link.replace(tagSuffix, "");

    return QUrl(link);
}

void OcsActivityJob::processNextPage(const QNetworkReply *reply)
{
    if (reply->hasRawHeader("Link")) {
        const auto nextLink = parseLinkFromHeader(reply->rawHeader("Link"));
        if (!nextLink.isValid()) {
            qCWarning(lcActivityJob) << "Link" << nextLink << "to next page submitted, but could not be parsed";
            emit error();
            return;
        }
        startNextJsonApiJob(nextLink);
    }
}

void OcsActivityJob::jsonApiJobFinished(const JsonApiJob &job, const QJsonDocument &json, int statusCode)
{
    if (statusCode != 200 && statusCode != 304) {
        qCWarning(lcActivityJob) << "Json api job finished with status code" << statusCode;
        emit error();
        return;
    }

    processActivities(json);
    processNextPage(job.reply());
}
}
