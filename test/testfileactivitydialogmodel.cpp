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

#include <memory>
#include <tuple>
#include <vector>

#include <QTest>
#include <QSignalSpy>
#include <QString>

#include "account.h"
#include "activityjob.h"
#include "fileactivitymodel.h"
#include "pushnotifications.h"
#include "tray/ActivityData.h"

#define VERIFY_ACTIVITY_JOB_QUERY_ACTIVITIES_CALLED_WITH(activityJob, index, objectType, objectId, limit) \
    const auto queryActivitiesCalls = (activityJob)->queryActivitiesCalls();                              \
    QVERIFY((index) < queryActivitiesCalls.size());                                                       \
    const auto queryAcitivitiesCall = queryActivitiesCalls[index];                                        \
    QCOMPARE(*std::get<0>(queryAcitivitiesCall), (objectType));                                           \
    QCOMPARE(*std::get<1>(queryAcitivitiesCall), (objectId));                                             \
    QCOMPARE(std::get<2>(queryAcitivitiesCall), (limit))

#define COMPARE_FILE_ACTIVITY(actualFileActivity, expectedId, expectedSubject, expectedFileAction, expectedDateTime) \
    QCOMPARE((actualFileActivity).id(), (expectedId));                                                               \
    QCOMPARE((actualFileActivity).message(), (expectedSubject));                                                     \
    QCOMPARE((actualFileActivity).type(), (expectedFileAction));                                                     \
    QCOMPARE((actualFileActivity).timestamp(), (expectedDateTime));

class FakeActivityJob : public OCC::ActivityJob
{
public:
    void queryActivities(OCC::Optional<QString> objectType, OCC::Optional<QString> objectId, int limit) override
    {
        _queryActivitiesCalls.emplace_back(objectType, objectId, limit);
        emit finished(_activities);
    }

    std::vector<std::tuple<OCC::Optional<QString>, OCC::Optional<QString>, int>> queryActivitiesCalls() const
    {
        return _queryActivitiesCalls;
    }

    void setActivities(const std::vector<OCC::Activity> &activities) { _activities = activities; }


private:
    std::vector<std::tuple<OCC::Optional<QString>, OCC::Optional<QString>, int>> _queryActivitiesCalls;
    std::vector<OCC::Activity> _activities;
};

static std::vector<OCC::Activity> createTwoActivities()
{
    const auto currentTime = QDateTime::currentDateTimeUtc();

    std::vector<OCC::Activity> activities;
    OCC::Activity a1;
    a1._id = 0;
    a1._subject = "subject1";
    a1._fileAction = "file_created";
    a1._dateTime = currentTime;
    activities.push_back(a1);
    OCC::Activity a2;
    a2._id = 1;
    a2._subject = "subject2";
    a2._fileAction = "file_changed";
    a2._dateTime = currentTime.addSecs(1);
    activities.push_back(a2);

    return activities;
}

class TestFileActivityDialogModel : public QObject
{
    Q_OBJECT

private slots:
    void testStart_fileIdGiven_queryActivities()
    {
        auto activityJobUniquePtr = std::make_unique<FakeActivityJob>();
        auto activityJob = activityJobUniquePtr.get();
        const auto activities = createTwoActivities();
        activityJob->setActivities(activities);
        OCC::FileActivityDialogModel fileActivityDialogModel(std::move(activityJobUniquePtr));
        QSignalSpy hideErrorSpy(&fileActivityDialogModel, &OCC::FileActivityDialogModel::hideError);
        QSignalSpy showErrorSpy(&fileActivityDialogModel, &OCC::FileActivityDialogModel::showError);
        QSignalSpy hideProgressSpy(&fileActivityDialogModel, &OCC::FileActivityDialogModel::hideProgress);
        QSignalSpy showProgressSpy(&fileActivityDialogModel, &OCC::FileActivityDialogModel::showProgress);
        QSignalSpy hideActivitiesSpy(&fileActivityDialogModel, &OCC::FileActivityDialogModel::hideActivities);
        QSignalSpy showActivitiesSpy(&fileActivityDialogModel, &OCC::FileActivityDialogModel::showActivities);

        const QString fileId("file_id");
        fileActivityDialogModel.start(fileId);
        // UI updated correct after a successful start?
        QCOMPARE(showErrorSpy.size(), 0);
        QCOMPARE(hideErrorSpy.size(), 1);
        QCOMPARE(showProgressSpy.size(), 1);
        QCOMPARE(hideProgressSpy.size(), 1);
        QCOMPARE(hideActivitiesSpy.size(), 1);
        QCOMPARE(showActivitiesSpy.size(), 1);

        // Check that activities once queried on startup
        QCOMPARE(activityJob->queryActivitiesCalls().size(), 1);
        VERIFY_ACTIVITY_JOB_QUERY_ACTIVITIES_CALLED_WITH(activityJob, 0, QLatin1String("files"), fileId, 50);

        const auto listModel = fileActivityDialogModel.getActivityListModel();
        QCOMPARE(listModel->rowCount(), 2);
        {
            QVERIFY(listModel->data(listModel->index(0)).canConvert<OCC::FileActivity>());
            const auto fileActivity = qvariant_cast<OCC::FileActivity>(listModel->data(listModel->index(0)));
            COMPARE_FILE_ACTIVITY(fileActivity, activities[1]._id, activities[1]._subject,
                OCC::FileActivity::Type::Changed, activities[1]._dateTime);
        }
        {
            QVERIFY(listModel->data(listModel->index(1)).canConvert<OCC::FileActivity>());
            const auto fileActivity = qvariant_cast<OCC::FileActivity>(listModel->data(listModel->index(1)));
            COMPARE_FILE_ACTIVITY(fileActivity, activities[0]._id, activities[0]._subject,
                OCC::FileActivity::Type::Created, activities[0]._dateTime);
        }
    }

    void test_insertedFileActivtyUpdated_displayFileActivitiesOrdered()
    {
        auto activityJobUniquePtr = std::make_unique<FakeActivityJob>();
        auto activityJob = activityJobUniquePtr.get();
        const auto activities = createTwoActivities();
        activityJob->setActivities(activities);
        OCC::FileActivityDialogModel fileActivityDialogModel(std::move(activityJobUniquePtr));

        const QString fileId("file_id");
        fileActivityDialogModel.start(fileId);

        // Are the activities once queried on startup?
        QCOMPARE(activityJob->queryActivitiesCalls().size(), 1);
        VERIFY_ACTIVITY_JOB_QUERY_ACTIVITIES_CALLED_WITH(activityJob, 0, QLatin1String("files"), fileId, 50);

        // Update a activity
        const auto currentTime = QDateTime::currentDateTimeUtc();
        OCC::FileActivity activity1Updated(activities[0]._id, activities[0]._subject, currentTime.addSecs(2),
            OCC::FileActivity::Type::Changed);
        const auto listModel = fileActivityDialogModel.getActivityListModel();
        listModel->addFileActivity(activity1Updated);

        // Was the activity inserted in the correct order?
        QCOMPARE(listModel->rowCount(), 2);
        {
            QVERIFY(listModel->data(listModel->index(0)).canConvert<OCC::FileActivity>());
            const auto fileActivity = qvariant_cast<OCC::FileActivity>(listModel->data(listModel->index(0)));
            COMPARE_FILE_ACTIVITY(fileActivity, activity1Updated.id(), activity1Updated.message(),
                activity1Updated.type(), activity1Updated.timestamp());
        }
        {
            QVERIFY(listModel->data(listModel->index(1)).canConvert<OCC::FileActivity>());
            const auto fileActivity = qvariant_cast<OCC::FileActivity>(listModel->data(listModel->index(1)));
            COMPARE_FILE_ACTIVITY(fileActivity, activities[1]._id, activities[1]._subject,
                OCC::FileActivity::Type::Changed, activities[1]._dateTime);
        }
    }

    void test_newFileActivtyAdded_displayFileActivitiesOrdered()
    {
        auto activityJobUniquePtr = std::make_unique<FakeActivityJob>();
        auto activityJob = activityJobUniquePtr.get();
        const auto activities = createTwoActivities();
        activityJob->setActivities(activities);
        OCC::FileActivityDialogModel fileActivityDialogModel(std::move(activityJobUniquePtr));

        const QString fileId("file_id");
        fileActivityDialogModel.start(fileId);

        // Are the activities once queried on startup?
        QCOMPARE(activityJob->queryActivitiesCalls().size(), 1);
        VERIFY_ACTIVITY_JOB_QUERY_ACTIVITIES_CALLED_WITH(activityJob, 0, QLatin1String("files"), fileId, 50);

        // Add a new activity
        const auto currentTime = QDateTime::currentDateTimeUtc();
        OCC::FileActivity newActivity(2, "subject3", currentTime.addSecs(2), OCC::FileActivity::Type::Changed);
        const auto listModel = fileActivityDialogModel.getActivityListModel();
        listModel->addFileActivity(newActivity);

        // Was the new activity inserted in the correct order?
        QCOMPARE(listModel->rowCount(), 3);
        {
            QVERIFY(listModel->data(listModel->index(0)).canConvert<OCC::FileActivity>());
            const auto fileActivity = qvariant_cast<OCC::FileActivity>(listModel->data(listModel->index(0)));
            COMPARE_FILE_ACTIVITY(fileActivity, newActivity.id(), newActivity.message(),
                newActivity.type(), newActivity.timestamp());
        }
        {
            QVERIFY(listModel->data(listModel->index(1)).canConvert<OCC::FileActivity>());
            const auto fileActivity = qvariant_cast<OCC::FileActivity>(listModel->data(listModel->index(1)));
            COMPARE_FILE_ACTIVITY(fileActivity, activities[1]._id, activities[1]._subject,
                OCC::FileActivity::Type::Changed, activities[1]._dateTime);
        }
        {
            QVERIFY(listModel->data(listModel->index(2)).canConvert<OCC::FileActivity>());
            const auto fileActivity = qvariant_cast<OCC::FileActivity>(listModel->data(listModel->index(2)));
            COMPARE_FILE_ACTIVITY(fileActivity, activities[0]._id, activities[0]._subject,
                OCC::FileActivity::Type::Created, activities[0]._dateTime);
        }
    }

    void test_noPushNotificationsGiven_queryActivitiesInRegularInterval()
    {
        auto activityJobUniquePtr = std::make_unique<FakeActivityJob>();
        auto activityJob = activityJobUniquePtr.get();
        const auto activities = createTwoActivities();
        activityJob->setActivities(activities);
        OCC::FileActivityDialogModel fileActivityDialogModel(std::move(activityJobUniquePtr));

        const QString fileId("file_id");
        fileActivityDialogModel.start(fileId);

        // Are the activities once queried on startup?
        QCOMPARE(activityJob->queryActivitiesCalls().size(), 1);

        // Activities polled in a regular Interval?
        fileActivityDialogModel.setActivityPollInterval(0);
        QVERIFY(QTest::qWaitFor([&]() {
            return activityJob->queryActivitiesCalls().size() >= 2;
        }));
    }

    void test_pushNotificationsGiven_queryActivitiesOnNotify()
    {
        auto activityJobUniquePtr = std::make_unique<FakeActivityJob>();
        auto activityJob = activityJobUniquePtr.get();
        const auto activities = createTwoActivities();
        activityJob->setActivities(activities);
        auto account = OCC::Account::create();
        OCC::PushNotifications pushNotifications(account.data());
        OCC::FileActivityDialogModel fileActivityDialogModel(std::move(activityJobUniquePtr), &pushNotifications);

        const QString fileId("file_id");
        fileActivityDialogModel.start(fileId);

        // Are the activities once queried on startup?
        QCOMPARE(activityJob->queryActivitiesCalls().size(), 1);

        emit pushNotifications.activitiesChanged(account.data());

        // Activities were queried again
        QCOMPARE(activityJob->queryActivitiesCalls().size(), 2);
    }

    void testShowError_activityJobEmittedError_showError()
    {
        auto activityJobUniquePtr = std::make_unique<FakeActivityJob>();
        auto activityJob = activityJobUniquePtr.get();
        const auto activities = createTwoActivities();
        activityJob->setActivities(activities);
        OCC::FileActivityDialogModel fileActivityDialogModel(std::move(activityJobUniquePtr));
        QSignalSpy showErrorSpy(&fileActivityDialogModel, &OCC::FileActivityDialogModel::showError);

        const QString fileId("file_id");
        fileActivityDialogModel.start(fileId);

        // Emit the error signal
        emit activityJob->error();

        // Does the error show up?
        QCOMPARE(showErrorSpy.size(), 1);
    }
};

QTEST_GUILESS_MAIN(TestFileActivityDialogModel)
#include "testfileactivitydialogmodel.moc"
