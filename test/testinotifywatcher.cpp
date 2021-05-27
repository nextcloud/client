/*
 *    This software is in the public domain, furnished "as is", without technical
 *       support, and with no warranty, express or implied, as to its usefulness for
 *          any purpose.
 *          */

#include <QtGlobal>
#include <QtTest>

#include "folderwatcher_linux.h"
#include "common/utility.h"

#define CHECKED_SYSTEM(cmd_)                  \
    do {                                      \
        int result_ = system(cmd_);           \
        Q_ASSERT(WIFEXITED(result_));         \
        Q_ASSERT(WEXITSTATUS(result_) == 0);  \
    } while(0)

using namespace OCC;

class TestInotifyWatcher: public FolderWatcherPrivate
{
    Q_OBJECT

private:
    QString _root;

private slots:
    void initTestCase() {
        qsrand(QTime::currentTime().msec());

        _root = QDir::tempPath() + "/" + "test_" + QString::number(qrand());
        qDebug() << "creating test directory tree in " << _root;
        QDir rootDir(_root);

        rootDir.mkpath(_root + "/a1/b1/c1");
        rootDir.mkpath(_root + "/a1/b1/c2");
        rootDir.mkpath(_root + "/a1/b2/c1");
        rootDir.mkpath(_root + "/a1/b3/c3");
        rootDir.mkpath(_root + "/a2/b3/c3");

    }

    // Test the recursive path listing function findFoldersBelow
    void testDirsBelowPath() {
        QStringList dirs;

        bool ok = findFoldersBelow(QDir(_root), dirs);
        QVERIFY( dirs.indexOf(_root + "/a1")>-1);
        QVERIFY( dirs.indexOf(_root + "/a1/b1")>-1);
        QVERIFY( dirs.indexOf(_root + "/a1/b1/c1")>-1);
        QVERIFY( dirs.indexOf(_root + "/a1/b1/c2")>-1);

        QVERIFY(Utility::writeRandomFile(_root+"/a1/rand1.dat"));
        QVERIFY(Utility::writeRandomFile(_root+"/a1/b1/rand2.dat"));
        QVERIFY(Utility::writeRandomFile(_root+"/a1/b1/c1/rand3.dat"));

        QVERIFY( dirs.indexOf(_root + "/a1/b2")>-1);
        QVERIFY( dirs.indexOf(_root + "/a1/b2/c1")>-1);
        QVERIFY( dirs.indexOf(_root + "/a1/b3")>-1);
        QVERIFY( dirs.indexOf(_root + "/a1/b3/c3")>-1);

        QVERIFY( dirs.indexOf(_root + "/a2"));
        QVERIFY( dirs.indexOf(_root + "/a2/b3"));
        QVERIFY( dirs.indexOf(_root + "/a2/b3/c3"));

        QVERIFY2(dirs.count() == 11, "Directory count wrong.");

        QVERIFY2(ok, "findFoldersBelow failed.");
    }

    void cleanupTestCase() {
        if( _root.startsWith(QDir::tempPath() )) {
            CHECKED_SYSTEM( QString("rm -rf %1").arg(_root).toLocal8Bit() );
        }
    }
};

QTEST_APPLESS_MAIN(TestInotifyWatcher)
#include "testinotifywatcher.moc"
