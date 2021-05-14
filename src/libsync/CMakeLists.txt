project(libsync)
include(DefinePlatformDefaults)

set(CMAKE_AUTOMOC TRUE)

if ( APPLE )
    list(APPEND OS_SPECIFIC_LINK_LIBRARIES
         /System/Library/Frameworks/CoreServices.framework
         /System/Library/Frameworks/Foundation.framework
         /System/Library/Frameworks/AppKit.framework
    )
endif()

set(libsync_SRCS
    account.cpp
    pushnotifications.cpp
    wordlist.cpp
    bandwidthmanager.cpp
    capabilities.cpp
    clientproxy.cpp
    cookiejar.cpp
    discovery.cpp
    discoveryphase.cpp
    encryptfolderjob.cpp
    filesystem.cpp
    httplogger.cpp
    logger.cpp
    accessmanager.cpp
    configfile.cpp
    abstractnetworkjob.cpp
    networkjobs.cpp
    owncloudpropagator.cpp
    nextcloudtheme.cpp
    abstractpropagateremotedeleteencrypted.cpp
    deletejob.cpp
    progressdispatcher.cpp
    propagatorjobs.cpp
    propagatedownload.cpp
    propagateupload.cpp
    propagateuploadv1.cpp
    propagateuploadng.cpp
    propagateremotedelete.cpp
    propagateremotedeleteencrypted.cpp
    propagateremotedeleteencryptedrootfolder.cpp
    propagateremotemove.cpp
    propagateremotemkdir.cpp
    propagateuploadencrypted.cpp
    propagatedownloadencrypted.cpp
    syncengine.cpp
    syncfileitem.cpp
    syncfilestatustracker.cpp
    localdiscoverytracker.cpp
    syncresult.cpp
    theme.cpp
    clientsideencryption.cpp
    clientsideencryptionjobs.cpp
    creds/dummycredentials.cpp
    creds/abstractcredentials.cpp
    creds/credentialscommon.cpp
    creds/keychainchunk.cpp
    vfs/suffix/vfs_suffix.cpp
)

if (WIN32)
    set(libsync_SRCS ${libsync_SRCS}
        vfs/cfapi/cfapiwrapper.cpp
        vfs/cfapi/hydrationjob.cpp
        vfs/cfapi/vfs_cfapi.cpp
    )
    add_definitions(-D_WIN32_WINNT=_WIN32_WINNT_WIN10)
    list(APPEND OS_SPECIFIC_LINK_LIBRARIES cldapi)
elseif(LINUX) # elseif(LINUX OR APPLE)
    set(libsync_SRCS ${libsync_SRCS} vfs/xattr/vfs_xattr.cpp)
    if (APPLE)
        set(libsync_SRCS ${libsync_SRCS} vfs/xattr/xattrwrapper_mac.cpp)
    else()
        set(libsync_SRCS ${libsync_SRCS} vfs/xattr/xattrwrapper_linux.cpp)
    endif()
endif()

if(TOKEN_AUTH_ONLY)
    set (libsync_SRCS ${libsync_SRCS} creds/tokencredentials.cpp)
else()
    set (libsync_SRCS ${libsync_SRCS} creds/httpcredentials.cpp)
endif()

# These headers are installed for libowncloudsync to be used by 3rd party apps
set(owncloudsync_HEADERS
    account.h
    syncengine.h
    configfile.h
    networkjobs.h
    progressdispatcher.h
    syncfileitem.h
    syncresult.h
)

set(creds_HEADERS
    creds/abstractcredentials.h
    creds/httpcredentials.h
)

IF (NOT APPLE)
    INSTALL(
        FILES ${owncloudsync_HEADERS}
	DESTINATION ${INCLUDE_INSTALL_DIR}/nextcloudsync/mirall
    )
    INSTALL(
        FILES ${creds_HEADERS}
	DESTINATION ${INCLUDE_INSTALL_DIR}/nextcloudsync/creds
    )
ENDIF(NOT APPLE)

find_package(Qt5 REQUIRED COMPONENTS WebSockets)
add_library(nextcloudsync SHARED ${libsync_SRCS})
set_target_properties(nextcloudsync PROPERTIES LIBRARY_OUTPUT_NAME ${APPLICATION_EXECUTABLE}sync)
target_link_libraries(nextcloudsync
  PUBLIC
  nextcloud_csync
  OpenSSL::Crypto
  OpenSSL::SSL
  ${OS_SPECIFIC_LINK_LIBRARIES}
  Qt5::Core
  Qt5::Network
  Qt5::WebSockets
)

if (NOT TOKEN_AUTH_ONLY)
    find_package(Qt5 REQUIRED COMPONENTS Widgets Svg)
    target_link_libraries(nextcloudsync PUBLIC Qt5::Widgets Qt5::Svg qt5keychain)
endif()

if(Inotify_FOUND)
    target_include_directories(nextcloudsync PRIVATE ${Inotify_INCLUDE_DIRS})
    target_link_libraries(nextcloudsync PUBLIC ${Inotify_LIBRARIES})
endif()

GENERATE_EXPORT_HEADER( nextcloudsync
	BASE_NAME nextcloudsync
	EXPORT_MACRO_NAME OWNCLOUDSYNC_EXPORT
        EXPORT_FILE_NAME owncloudlib.h
	STATIC_DEFINE OWNCLOUD_BUILT_AS_STATIC
)

target_include_directories(nextcloudsync PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})


set_target_properties( nextcloudsync  PROPERTIES
	VERSION ${MIRALL_VERSION}
	SOVERSION ${MIRALL_SOVERSION}
	RUNTIME_OUTPUT_DIRECTORY ${BIN_OUTPUT_DIRECTORY}
)

if(NOT BUILD_OWNCLOUD_OSX_BUNDLE)
    install(TARGETS nextcloudsync
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
else()
    install(TARGETS nextcloudsync DESTINATION ${OWNCLOUD_OSX_BUNDLE}/Contents/MacOS)
endif()
