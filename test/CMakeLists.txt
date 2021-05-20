include(DefinePlatformDefaults)

include(nextcloud_add_test.cmake)

set(CMAKE_AUTOMOC TRUE)

add_library(testutils
  STATIC
  syncenginetestutils.cpp
  pushnotificationstestutils.cpp
  themeutils.cpp
)

target_link_libraries(testutils PUBLIC ${APPLICATION_EXECUTABLE}sync Qt5::Test)
target_include_directories(testutils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(testutils PROPERTIES FOLDER Tests)

nextcloud_add_test(NextcloudPropagator)

IF(BUILD_UPDATER)
    nextcloud_add_test(Updater)
endif()

nextcloud_add_test(NetrcParser)
nextcloud_add_test(OwnSql)
nextcloud_add_test(SyncJournalDB)
nextcloud_add_test(SyncFileItem)
nextcloud_add_test(ConcatUrl)
nextcloud_add_test(Cookies)
nextcloud_add_test(XmlParse)
nextcloud_add_test(ChecksumValidator)

nextcloud_add_test(ClientSideEncryption)
nextcloud_add_test(ExcludedFiles)

nextcloud_add_test(Utility)
nextcloud_add_test(SyncEngine)
nextcloud_add_test(SyncVirtualFiles)
nextcloud_add_test(SyncMove)
nextcloud_add_test(SyncDelete)
nextcloud_add_test(SyncConflict)
nextcloud_add_test(SyncFileStatusTracker)
nextcloud_add_test(Download)
nextcloud_add_test(ChunkingNg)
nextcloud_add_test(AsyncOp)
nextcloud_add_test(UploadReset)
nextcloud_add_test(AllFilesDeleted)
nextcloud_add_test(Blacklist)
nextcloud_add_test(LocalDiscovery)
nextcloud_add_test(RemoteDiscovery)
nextcloud_add_test(Permissions)
nextcloud_add_test(SelectiveSync)
nextcloud_add_test(DatabaseError)
nextcloud_add_test(LockedFiles)
nextcloud_add_test(FolderWatcher)
nextcloud_add_test(Capabilities)
nextcloud_add_test(PushNotifications)
nextcloud_add_test(Theme)
nextcloud_add_test(NotificationCache)
nextcloud_add_test(FileActivityDialogModel)

if( UNIX AND NOT APPLE )
    nextcloud_add_test(InotifyWatcher)
endif(UNIX AND NOT APPLE)

if (WIN32)
    nextcloud_add_test(LongWinPath)
    nextcloud_add_test(SyncCfApi)
elseif(LINUX) # elseif(LINUX OR APPLE)
    nextcloud_add_test(SyncXAttr)
endif()

nextcloud_add_benchmark(LargeSync)

nextcloud_add_test(FolderMan)
nextcloud_add_test(RemoteWipe)

nextcloud_add_test(OAuth)

configure_file(test_journal.db "${PROJECT_BINARY_DIR}/bin/test_journal.db" COPYONLY)

find_package(CMocka)
if (CMOCKA_FOUND)
    include(AddCMockaTest)
    add_subdirectory(csync)
endif (CMOCKA_FOUND)
