project(libsync)
set(CMAKE_AUTOMOC TRUE)

if ( APPLE )
    list(APPEND OS_SPECIFIC_LINK_LIBRARIES
         /System/Library/Frameworks/CoreServices.framework
         /System/Library/Frameworks/Foundation.framework
         /System/Library/Frameworks/AppKit.framework
    )
endif()

IF(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD|NetBSD|OpenBSD")
    list(APPEND OS_SPECIFIC_LINK_LIBRARIES
	inotify
    )
ENDIF(${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD|NetBSD|OpenBSD")

if(SPARKLE_FOUND AND NOT BUILD_LIBRARIES_ONLY)
    list (APPEND OS_SPECIFIC_LINK_LIBRARIES ${SPARKLE_LIBRARY})
endif()

set(libsync_SRCS
    account.cpp
    wordlist.cpp
    bandwidthmanager.cpp
    capabilities.cpp
    cookiejar.cpp
    discoveryphase.cpp
    filesystem.cpp
    logger.cpp
    accessmanager.cpp
    configfile.cpp
    abstractnetworkjob.cpp
    networkjobs.cpp
    owncloudpropagator.cpp
    nextcloudtheme.cpp
    progressdispatcher.cpp
    propagatorjobs.cpp
    propagatedownload.cpp
    propagateupload.cpp
    propagateuploadv1.cpp
    propagateuploadng.cpp
    propagateremotedelete.cpp
    propagateremotedeleteencrypted.cpp
    propagateremotemove.cpp
    propagateremotemkdir.cpp
    propagateuploadencrypted.cpp
    propagatedownloadencrypted.cpp
    syncengine.cpp
    syncfileitem.cpp
    syncfilestatus.cpp
    syncfilestatustracker.cpp
    syncresult.cpp
    theme.cpp
    creds/dummycredentials.cpp
    creds/abstractcredentials.cpp
    creds/credentialscommon.cpp
)

if(TOKEN_AUTH_ONLY)
    set (libsync_SRCS ${libsync_SRCS} creds/tokencredentials.cpp)
else()
    set (libsync_SRCS ${libsync_SRCS} creds/httpcredentials.cpp
        clientsideencryption.cpp
        clientsideencryptionjobs.cpp
    )
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
	DESTINATION ${INCLUDE_INSTALL_DIR}/${synclib_NAME}/mirall
    )
    INSTALL(
        FILES ${creds_HEADERS}
	DESTINATION ${INCLUDE_INSTALL_DIR}/${synclib_NAME}/creds
    )
ENDIF(NOT APPLE)

add_library(${synclib_NAME} SHARED ${libsync_SRCS})
target_link_libraries(${synclib_NAME}
    ocsync
    OpenSSL::Crypto
    OpenSSL::SSL
    ${OS_SPECIFIC_LINK_LIBRARIES}
    Qt5::Core Qt5::Network
)

if (NOT TOKEN_AUTH_ONLY)
    find_package(Qt5 REQUIRED COMPONENTS Widgets Svg)
    target_link_libraries(${synclib_NAME} Qt5::Widgets Qt5::Svg ${QTKEYCHAIN_LIBRARY})
    target_include_directories(${synclib_NAME} PRIVATE ${QTKEYCHAIN_INCLUDE_DIR})
endif()

if(INOTIFY_FOUND)
    target_include_directories(${synclib_NAME} PRIVATE ${INOTIFY_INCLUDE_DIR})
    link_directories(${INOTIFY_LIBRARY_DIR})
    target_link_libraries(${synclib_NAME} ${INOTIFY_LIBRARY} )
endif()

GENERATE_EXPORT_HEADER( ${synclib_NAME}
	BASE_NAME ${synclib_NAME}
	EXPORT_MACRO_NAME OWNCLOUDSYNC_EXPORT
        EXPORT_FILE_NAME owncloudlib.h
	STATIC_DEFINE OWNCLOUD_BUILT_AS_STATIC
)

target_include_directories(${synclib_NAME} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})


set_target_properties( ${synclib_NAME}  PROPERTIES
	VERSION ${MIRALL_VERSION}
	SOVERSION ${MIRALL_SOVERSION}
	RUNTIME_OUTPUT_DIRECTORY ${BIN_OUTPUT_DIRECTORY}
)
set_target_properties( ${synclib_NAME} PROPERTIES
	INSTALL_RPATH "${CMAKE_INSTALL_PREFIX}/${LIB_INSTALL_DIR}/${APPLICATION_EXECUTABLE};${CMAKE_INSTALL_RPATH}" )

if(NOT BUILD_OWNCLOUD_OSX_BUNDLE)
    install(TARGETS ${synclib_NAME}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
else()
    install(TARGETS ${synclib_NAME} DESTINATION ${OWNCLOUD_OSX_BUNDLE}/Contents/MacOS)
    if (SPARKLE_FOUND)
        install(DIRECTORY "${SPARKLE_LIBRARY}"
	        DESTINATION "${OWNCLOUD_OSX_BUNDLE}/Contents/Frameworks" USE_SOURCE_PERMISSIONS)
    endif (SPARKLE_FOUND)
endif()


