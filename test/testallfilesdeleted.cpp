/*
 *    This software is in the public domain, furnished "as is", without technical
 *    support, and with no warranty, express or implied, as to its usefulness for
 *    any purpose.
 *
 */

#include <QtTest>
#include "syncenginetestutils.h"
#include <syncengine.h>
#include <configfile.h>

using namespace OCC;


static void changeAllFileId(FileInfo &info) {
    info.fileId = generateFileId();
    if (!info.isDir)
        return;
    info.etag = generateEtag();
    for (auto &child : info.children) {
        changeAllFileId(child);
    }
}

/*
 * This test ensure that the SyncEngine::aboutToRemoveAllFiles is correctly called and that when
 * we the user choose to remove all files SyncJournalDb::clearFileTable makes works as expected
 */
class TestAllFilesDeleted : public QObject
{
    Q_OBJECT

private slots:

    void testAllFilesDeletedKeep_data()
    {
        QTest::addColumn<bool>("deleteOnRemote");
        QTest::newRow("local") << false;
        QTest::newRow("remote") << true;

    }

    /*
     * In this test, all files are deleted in the client, or the server, and we simulate
     * that the users press "keep"
     */
    void testAllFilesDeletedKeep()
    {
        QFETCH(bool, deleteOnRemote);
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        ConfigFile config;
        config.setPromptDeleteFiles(true);

        //Just set a blacklist so we can check it is still there. This directory does not exists but
        // that does not matter for our purposes.
        QStringList selectiveSyncBlackList = { "Q/" };
        fakeFolder.syncEngine().journal()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList,
                                                                selectiveSyncBlackList);

        auto initialState = fakeFolder.currentLocalState();
        int aboutToRemoveAllFilesCalled = 0;
        QObject::connect(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles,
            [&](SyncFileItem::Direction dir, std::function<void(bool)> callback) {
                QCOMPARE(aboutToRemoveAllFilesCalled, 0);
                aboutToRemoveAllFilesCalled++;
                QCOMPARE(dir, deleteOnRemote ? SyncFileItem::Down : SyncFileItem::Up);
                callback(true);
                fakeFolder.syncEngine().journal()->clearFileTable(); // That's what Folder is doing
            });

        auto &modifier = deleteOnRemote ? fakeFolder.remoteModifier() : fakeFolder.localModifier();
        for (const auto &s : fakeFolder.currentRemoteState().children.keys())
            modifier.remove(s);

        QVERIFY(!fakeFolder.syncOnce()); // Should fail because we cancel the sync
        QCOMPARE(aboutToRemoveAllFilesCalled, 1);

        // Next sync should recover all files
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), initialState);
        QCOMPARE(fakeFolder.currentRemoteState(), initialState);

        // The selective sync blacklist should be not have been deleted.
        bool ok = true;
        QCOMPARE(fakeFolder.syncEngine().journal()->getSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList, &ok),
                 selectiveSyncBlackList);
    }

    void testAllFilesDeletedDelete_data()
    {
        testAllFilesDeletedKeep_data();
    }

    /*
     * This test is like the previous one but we simulate that the user presses "delete"
     */
    void testAllFilesDeletedDelete()
    {
        QFETCH(bool, deleteOnRemote);
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};

        int aboutToRemoveAllFilesCalled = 0;
        QObject::connect(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles,
            [&](SyncFileItem::Direction dir, std::function<void(bool)> callback) {
                QCOMPARE(aboutToRemoveAllFilesCalled, 0);
                aboutToRemoveAllFilesCalled++;
                QCOMPARE(dir, deleteOnRemote ? SyncFileItem::Down : SyncFileItem::Up);
                callback(false);
            });

        auto &modifier = deleteOnRemote ? fakeFolder.remoteModifier() : fakeFolder.localModifier();
        for (const auto &s : fakeFolder.currentRemoteState().children.keys())
            modifier.remove(s);

        QVERIFY(fakeFolder.syncOnce()); // Should succeed, and all files must then be deleted

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(fakeFolder.currentLocalState().children.count(), 0);

        // Try another sync to be sure.

        QVERIFY(fakeFolder.syncOnce()); // Should succeed (doing nothing)
        QCOMPARE(aboutToRemoveAllFilesCalled, 1); // should not have been called.

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(fakeFolder.currentLocalState().children.count(), 0);
    }

    void testNotDeleteMetaDataChange() {
        /**
         * This test make sure that we don't popup a file deleted message if all the metadata have
         * been updated (for example when the server is upgraded or something)
         **/

        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};
        // We never remove all files.
        QObject::connect(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles,
            [&] { QVERIFY(false); });
        QVERIFY(fakeFolder.syncOnce());

        for (const auto &s : fakeFolder.currentRemoteState().children.keys())
            fakeFolder.syncJournal().avoidRenamesOnNextSync(s); // clears all the fileid and inodes.
        fakeFolder.localModifier().remove("A/a1");
        auto expectedState = fakeFolder.currentLocalState();
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), expectedState);
        QCOMPARE(fakeFolder.currentRemoteState(), expectedState);

        fakeFolder.remoteModifier().remove("B/b1");
        changeAllFileId(fakeFolder.remoteModifier());
        expectedState = fakeFolder.currentRemoteState();
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), expectedState);
        QCOMPARE(fakeFolder.currentRemoteState(), expectedState);
    }

    void testResetServer()
    {
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};

        int aboutToRemoveAllFilesCalled = 0;
        QObject::connect(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles,
            [&](SyncFileItem::Direction dir, std::function<void(bool)> callback) {
                QCOMPARE(aboutToRemoveAllFilesCalled, 0);
                aboutToRemoveAllFilesCalled++;
                QCOMPARE(dir, SyncFileItem::Down);
                callback(false);
            });

        // Some small changes
        fakeFolder.localModifier().mkdir("Q");
        fakeFolder.localModifier().insert("Q/q1");
        fakeFolder.localModifier().appendByte("B/b1");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(aboutToRemoveAllFilesCalled, 0);

        // Do some change localy
        fakeFolder.localModifier().appendByte("A/a1");

        // reset the server.
        fakeFolder.remoteModifier() = FileInfo::A12_B12_C12_S12();

        // Now, aboutToRemoveAllFiles with down as a direction
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(aboutToRemoveAllFilesCalled, 1);

    }

    void testDataFingetPrint_data()
    {
        QTest::addColumn<bool>("hasInitialFingerPrint");
        QTest::newRow("initial finger print") << true;
        QTest::newRow("no initial finger print") << false;
    }

    void testDataFingetPrint()
    {
        QFETCH(bool, hasInitialFingerPrint);
        FakeFolder fakeFolder{ FileInfo::A12_B12_C12_S12() };
        fakeFolder.remoteModifier().setContents("C/c1", 'N');
        fakeFolder.remoteModifier().setModTime("C/c1", QDateTime::currentDateTimeUtc().addDays(-2));
        fakeFolder.remoteModifier().remove("C/c2");
        if (hasInitialFingerPrint) {
            fakeFolder.remoteModifier().extraDavProperties = "<oc:data-fingerprint>initial_finger_print</oc:data-fingerprint>";
        } else {
            //Server support finger print, but none is set.
            fakeFolder.remoteModifier().extraDavProperties = "<oc:data-fingerprint></oc:data-fingerprint>";
        }

        int fingerprintRequests = 0;
        fakeFolder.setServerOverride([&](QNetworkAccessManager::Operation, const QNetworkRequest &request, QIODevice *stream) -> QNetworkReply * {
            auto verb = request.attribute(QNetworkRequest::CustomVerbAttribute);
            if (verb == "PROPFIND") {
                auto data = stream->readAll();
                if (data.contains("data-fingerprint")) {
                    if (request.url().path().endsWith("dav/"))
                        ++fingerprintRequests;
                    else
                        fingerprintRequests = -10000; // fingerprint queried on incorrect path
                }
            }
            return nullptr;
        });

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fingerprintRequests, 1);
        // First sync, we did not change the finger print, so the file should be downloaded as normal
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
        QCOMPARE(fakeFolder.currentRemoteState().find("C/c1")->contentChar, 'N');
        QVERIFY(!fakeFolder.currentRemoteState().find("C/c2"));

        /* Simulate a backup restoration */

        // A/a1 is an old file
        fakeFolder.remoteModifier().setContents("A/a1", 'O');
        fakeFolder.remoteModifier().setModTime("A/a1", QDateTime::currentDateTimeUtc().addDays(-2));
        // B/b1 did not exist at the time of the backup
        fakeFolder.remoteModifier().remove("B/b1");
        // B/b2 was uploaded by another user in the mean time.
        fakeFolder.remoteModifier().setContents("B/b2", 'N');
        fakeFolder.remoteModifier().setModTime("B/b2", QDateTime::currentDateTimeUtc().addDays(2));

        // C/c3 was removed since we made the backup
        fakeFolder.remoteModifier().insert("C/c3_removed");
        // C/c4 was moved to A/a2 since we made the backup
        fakeFolder.remoteModifier().rename("A/a2", "C/old_a2_location");

        // The admin sets the data-fingerprint property
        fakeFolder.remoteModifier().extraDavProperties = "<oc:data-fingerprint>new_finger_print</oc:data-fingerprint>";

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fingerprintRequests, 2);
        auto currentState = fakeFolder.currentLocalState();
        // Altough the local file is kept as a conflict, the server file is downloaded
        QCOMPARE(currentState.find("A/a1")->contentChar, 'O');
        auto conflict = findConflict(currentState, "A/a1");
        QVERIFY(conflict);
        QCOMPARE(conflict->contentChar, 'W');
        fakeFolder.localModifier().remove(conflict->path());
        // b1 was restored (re-uploaded)
        QVERIFY(currentState.find("B/b1"));

        // b2 has the new content (was not restored), since its mode time goes forward in time
        QCOMPARE(currentState.find("B/b2")->contentChar, 'N');
        conflict = findConflict(currentState, "B/b2");
        QVERIFY(conflict); // Just to be sure, we kept the old file in a conflict
        QCOMPARE(conflict->contentChar, 'W');
        fakeFolder.localModifier().remove(conflict->path());

        // We actually do not remove files that technically should have been removed (we don't want data-loss)
        QVERIFY(currentState.find("C/c3_removed"));
        QVERIFY(currentState.find("C/old_a2_location"));

        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    void testSingleFileRenamed() {
        FakeFolder fakeFolder{FileInfo{}};

        int aboutToRemoveAllFilesCalled = 0;
        QObject::connect(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles,
            [&](SyncFileItem::Direction , std::function<void(bool)> ) {
                aboutToRemoveAllFilesCalled++;
                QFAIL("should not be called");
            });

        // add a single file
        fakeFolder.localModifier().insert("hello.txt");
        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(aboutToRemoveAllFilesCalled, 0);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        // rename it
        fakeFolder.localModifier().rename("hello.txt", "goodbye.txt");

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(aboutToRemoveAllFilesCalled, 0);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());
    }

    void testSelectiveSyncNoPopup() {
        // Unselecting all folder should not cause the popup to be shown
        FakeFolder fakeFolder{FileInfo::A12_B12_C12_S12()};

        int aboutToRemoveAllFilesCalled = 0;
        QObject::connect(&fakeFolder.syncEngine(), &SyncEngine::aboutToRemoveAllFiles,
            [&](SyncFileItem::Direction , std::function<void(bool)>) {
                aboutToRemoveAllFilesCalled++;
                QFAIL("should not be called");
            });

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(aboutToRemoveAllFilesCalled, 0);
        QCOMPARE(fakeFolder.currentLocalState(), fakeFolder.currentRemoteState());

        fakeFolder.syncEngine().journal()->setSelectiveSyncList(SyncJournalDb::SelectiveSyncBlackList,
            QStringList() << "A/" << "B/" << "C/" << "S/");

        QVERIFY(fakeFolder.syncOnce());
        QCOMPARE(fakeFolder.currentLocalState(), FileInfo{}); // all files should be one localy
        QCOMPARE(fakeFolder.currentRemoteState(), FileInfo::A12_B12_C12_S12()); // Server not changed
        QCOMPARE(aboutToRemoveAllFilesCalled, 0); // But we did not show the popup
    }


};

QTEST_GUILESS_MAIN(TestAllFilesDeleted)
#include "testallfilesdeleted.moc"
