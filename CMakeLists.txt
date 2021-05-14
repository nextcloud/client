cmake_minimum_required(VERSION 3.6)
set(CMAKE_CXX_STANDARD 14)

project(client)

include(FeatureSummary)

set(BIN_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")


set(OEM_THEME_DIR "" CACHE STRING "Define directory containing a custom theme")
if ( EXISTS ${OEM_THEME_DIR}/OEM.cmake )
     include ( ${OEM_THEME_DIR}/OEM.cmake )
else ()
     include ( ${CMAKE_SOURCE_DIR}/NEXTCLOUD.cmake )
endif()

# Default suffix if the theme doesn't define one
if(NOT DEFINED APPLICATION_VIRTUALFILE_SUFFIX)
    set(APPLICATION_VIRTUALFILE_SUFFIX "${APPLICATION_SHORTNAME}_virtual" CACHE STRING "Virtual file suffix (not including the .)")
endif()

# need this logic to not mess with re/uninstallations via macosx.pkgproj
if(${APPLICATION_REV_DOMAIN} STREQUAL "com.owncloud.desktopclient")
    set(APPLICATION_REV_DOMAIN_INSTALLER "com.ownCloud.client")
else()
    set(APPLICATION_REV_DOMAIN_INSTALLER ${APPLICATION_REV_DOMAIN})
endif()

# For usage in XML files we preprocess
string(REPLACE "&" "&amp;" APPLICATION_NAME_XML_ESCAPED "${APPLICATION_NAME}")
string(REPLACE "<" "&lt;" APPLICATION_NAME_XML_ESCAPED "${APPLICATION_NAME_XML_ESCAPED}")
string(REPLACE ">" "&gt;" APPLICATION_NAME_XML_ESCAPED "${APPLICATION_NAME_XML_ESCAPED}")

if (NOT DEFINED LINUX_PACKAGE_SHORTNAME)
    set(LINUX_PACKAGE_SHORTNAME "${APPLICATION_SHORTNAME}")
endif()

if (NOT DEFINED PACKAGE)
    set(PACKAGE "${LINUX_PACKAGE_SHORTNAME}-client")
endif()

set( CMAKE_MODULE_PATH ${CMAKE_SOURCE_DIR}/cmake/modules )

if(NOT CRASHREPORTER_EXECUTABLE)
    set(CRASHREPORTER_EXECUTABLE "${APPLICATION_EXECUTABLE}_crash_reporter")
endif()

include(Warnings)

include(${CMAKE_SOURCE_DIR}/VERSION.cmake)
# For config.h
include_directories(BEFORE ${CMAKE_CURRENT_BINARY_DIR})
# Allows includes based on src/ like #include "common/utility.h" or #include "csync/csync.h"
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/src
)

# disable the crashreporter if libcrashreporter-qt is not available or we're building for ARM
if( CMAKE_SYSTEM_PROCESSOR MATCHES "arm" OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/3rdparty/libcrashreporter-qt/CMakeLists.txt")
    set( WITH_CRASHREPORTER OFF )
endif()

if(NOT WITH_CRASHREPORTER)
    message(STATUS "Build of crashreporter disabled.")
endif()

include(GNUInstallDirs)
include(DefineInstallationPaths)
include(GenerateExportHeader)

include(GetGitRevisionDescription)

get_git_head_revision(GIT_REFSPEC GIT_SHA1)

add_definitions(
    -DQT_USE_QSTRINGBUILDER
    -DQT_MESSAGELOGCONTEXT #enable function name and line number in debug output
    -DQT_DEPRECATED_WARNINGS
)

# if we cannot get it from git, directly try .tag (packages)
# this will work if the tar balls have been properly created
# via git-archive.
if ("${GIT_SHA1}" STREQUAL "GITDIR-NOTFOUND")
    file(READ ${CMAKE_SOURCE_DIR}/.tag sha1_candidate)
    string(REPLACE "\n" "" sha1_candidate ${sha1_candidate})
    if (NOT ${sha1_candidate} STREQUAL "$Format:%H$")
        message("${sha1_candidate}")
        set (GIT_SHA1 "${sha1_candidate}")
    endif()
endif()
message(STATUS "GIT_SHA1 ${GIT_SHA1}")

set(SYSCONFDIR ${SYSCONF_INSTALL_DIR})
set(DATADIR ${DATA_INSTALL_DIR})
if(WIN32)
set(DATADIR "share")
endif(WIN32)
set(SHAREDIR ${DATADIR})

#####
## handle BUILD_OWNCLOUD_OSX_BUNDLE
# BUILD_OWNCLOUD_OSX_BUNDLE was not initialized OR set to true on OSX
if(APPLE AND (NOT DEFINED BUILD_OWNCLOUD_OSX_BUNDLE OR BUILD_OWNCLOUD_OSX_BUNDLE))
    set(BUILD_OWNCLOUD_OSX_BUNDLE ON)
    set(OWNCLOUD_OSX_BUNDLE "${APPLICATION_EXECUTABLE}.app")
    set(LIB_INSTALL_DIR "${APPLICATION_EXECUTABLE}.app/Contents/MacOS")
    set(BIN_INSTALL_DIR "${APPLICATION_EXECUTABLE}.app/Contents/MacOS")

# BUILD_OWNCLOUD_OSX_BUNDLE was disabled on OSX
elseif(APPLE AND NOT BUILD_OWNCLOUD_OSX_BUNDLE)
    message(FATAL_ERROR "Building in non-bundle mode on OSX is currently not supported. Comment this error out if you want to work on/test it.")

# any other platform
else()
    set(BUILD_OWNCLOUD_OSX_BUNDLE OFF)
endif()
#####

# this option removes Http authentication, keychain, shibboleth etc and is intended for
# external authentication mechanisms
option(TOKEN_AUTH_ONLY "TOKEN_AUTH_ONLY" OFF)
if(TOKEN_AUTH_ONLY)
   message("Compiling with token authentication")
   add_definitions(-DTOKEN_AUTH_ONLY=1)
endif()

option(NO_MSG_HANDLER "Don't redirect QDebug outputs to the log window/file" OFF)
if(NO_MSG_HANDLER)
   add_definitions(-DNO_MSG_HANDLER=1)
endif()

# this option builds the updater
option(BUILD_UPDATER "BUILD_UPDATER" OFF)
if(BUILD_UPDATER)
   message("Compiling with updater")
   add_definitions(-DBUILD_UPDATER=1)
else()
   message("Compiling without updater")
endif()

# this option builds the shell integration
option(BUILD_SHELL_INTEGRATION "BUILD_SHELL_INTEGRATION" ON)

# this option builds/installs the generic shell integration icons
option(BUILD_SHELL_INTEGRATION_ICONS "BUILD_SHELL_INTEGRATION_ICONS" ON)

# this options builds the dolphin integration plugin
option(BUILD_SHELL_INTEGRATION_DOLPHIN "BUILD_SHELL_INTEGRATION_DOLPHIN" ON)

# this options builds the nautilus (like) integration plugins
option(BUILD_SHELL_INTEGRATION_NAUTILUS "BUILD_SHELL_INTEGRATION_NAUTILUS" ON)

# this option builds the client
option(BUILD_CLIENT "BUILD_CLIENT" ON)

# this option creates only libocsync and libowncloudsync (NOTE: BUILD_CLIENT needs to be on)
option(BUILD_LIBRARIES_ONLY "BUILD_LIBRARIES_ONLY" OFF)

# build the GUI component, when disabled only nextcloudcmd is built
option(BUILD_GUI "BUILD_GUI" ON)

# When this option is enabled, 5xx errors are not added to the blacklist
# Normally you don't want to enable this option because if a particular file
# triggers a bug on the server, you want the file to be blacklisted.
option(OWNCLOUD_5XX_NO_BLACKLIST "OWNCLOUD_5XX_NO_BLACKLIST" OFF)
if(OWNCLOUD_5XX_NO_BLACKLIST)
    add_definitions(-DOWNCLOUD_5XX_NO_BLACKLIST=1)
endif()

if(APPLE)
  set( SOCKETAPI_TEAM_IDENTIFIER_PREFIX "" CACHE STRING "SocketApi prefix (including a following dot) that must match the codesign key's TeamIdentifier/Organizational Unit" )
endif()

if(BUILD_CLIENT)
    OPTION(GUI_TESTING "Build with gui introspection features of socket api" OFF)

    if(APPLE AND BUILD_UPDATER)
        find_package(Sparkle)
    endif()

    if(UNIX AND NOT APPLE)
        find_package(Inotify REQUIRED)
    endif()
    find_package(Sphinx)
    find_package(PdfLatex)
    find_package(OpenSSL 1.1 REQUIRED )

   find_package(ZLIB REQUIRED)

   if(NOT WIN32 AND NOT APPLE)
      find_package(PkgConfig REQUIRED)
      pkg_check_modules(CLOUDPROVIDERS cloudproviders IMPORTED_TARGET)

      if(CLOUDPROVIDERS_FOUND)
        pkg_check_modules(GIO REQUIRED gio-2.0 IMPORTED_TARGET)
        pkg_check_modules(GLIB2 REQUIRED glib-2.0 IMPORTED_TARGET)
      endif()
   endif()
endif()

if (NOT DEFINED APPLICATION_ICON_NAME)
    set(APPLICATION_ICON_NAME ${APPLICATION_SHORTNAME})
endif()

include(NextcloudCPack.cmake)

add_definitions(-DUNICODE)
add_definitions(-D_UNICODE)
if( WIN32 )
add_definitions( -D__USE_MINGW_ANSI_STDIO=1 )
add_definitions( -DNOMINMAX )
# Get APIs from from Vista onwards.
add_definitions(-D_WIN32_WINNT=0x0601)
add_definitions(-DWINVER=0x0601)
add_definitions(-DNTDDI_VERSION=0x0A000004)
    if( MSVC )
    # Use automatic overload for suitable CRT safe-functions
    # See https://docs.microsoft.com/de-de/cpp/c-runtime-library/security-features-in-the-crt?view=vs-2019
    add_definitions( -D_CRT_SECURE_CPP_OVERLOAD_STANDARD_NAMES=1 )
    # Also: Disable compiler warnings because we don't use Windows CRT safe-functions explicitly and don't intend to
    # as this is a pure cross-platform source the only alternative would be a ton of ifdefs with calls to the _s version
    add_definitions( -D_CRT_SECURE_NO_WARNINGS )
    endif( MSVC )
endif( WIN32 )

if (APPLE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -stdlib=libc++")
endif()

include(SanitizerFlags)

# Handle Translations, pick all client_* files from trans directory.
file( GLOB TRANS_FILES ${CMAKE_SOURCE_DIR}/translations/client_*.ts)
set(TRANSLATIONS ${TRANS_FILES})

if(BUILD_CLIENT)
    add_subdirectory(src)
    if(NOT BUILD_LIBRARIES_ONLY)
        add_subdirectory(man)
        add_subdirectory(doc)
        add_subdirectory(doc/dev)
        if(IS_DIRECTORY ${CMAKE_SOURCE_DIR}/admin)
            add_subdirectory(admin)
        endif(IS_DIRECTORY ${CMAKE_SOURCE_DIR}/admin)
    endif(NOT BUILD_LIBRARIES_ONLY)
endif()

if(BUILD_SHELL_INTEGRATION)
    add_subdirectory(shell_integration)
endif()

if(BUILD_TESTING)
    include(CTest)
    enable_testing()
    add_subdirectory(test)
endif()

configure_file(config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)
configure_file(version.h.in ${CMAKE_CURRENT_BINARY_DIR}/version.h)

if(BUILD_OWNCLOUD_OSX_BUNDLE)
    install(FILES sync-exclude.lst DESTINATION ${OWNCLOUD_OSX_BUNDLE}/Contents/Resources/)
    configure_file(sync-exclude.lst bin/${OWNCLOUD_OSX_BUNDLE}/Contents/Resources/sync-exclude.lst COPYONLY)
elseif(BUILD_CLIENT)
    install( FILES sync-exclude.lst DESTINATION ${SYSCONFDIR}/${APPLICATION_SHORTNAME} )
    configure_file(sync-exclude.lst bin/sync-exclude.lst COPYONLY)
endif()

feature_summary(WHAT ALL FATAL_ON_MISSING_REQUIRED_PACKAGES INCLUDE_QUIET_PACKAGES)
