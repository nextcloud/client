project(libcsync)
set(CMAKE_AUTOMOC TRUE)
add_definitions(-DQT_NO_CAST_TO_ASCII
                -DQT_NO_CAST_FROM_ASCII
                -DQT_NO_URL_CAST_FROM_STRING
                -DQT_NO_CAST_FROM_BYTEARRAY)

# global needed variables
set(APPLICATION_NAME "ocsync")

set(LIBRARY_VERSION ${MIRALL_VERSION})
set(LIBRARY_SOVERSION "0")

# add definitions
include(DefineCMakeDefaults)
include(DefinePlatformDefaults)
include(DefineCompilerFlags)
include(DefineOptions.cmake)

include(DefineInstallationPaths)

if (APPLE OR WIN32)
    set(USE_OUR_OWN_SQLITE3 TRUE)
else()
    find_package(SQLite3 3.8.0 REQUIRED)
endif()

include(ConfigureChecks.cmake)
include(../common/common.cmake)

if (MEM_NULL_TESTS)
  add_definitions(-DCSYNC_MEM_NULL_TESTS)
endif (MEM_NULL_TESTS)

# Specific option for builds tied to servers that do not support renaming extensions
set(NO_RENAME_EXTENSION 0 CACHE BOOL "Do not issue rename if the extension changes")
if(NO_RENAME_EXTENSION)
    add_definitions(-DNO_RENAME_EXTENSION)
endif()

set(csync_SRCS
  csync.cpp
  csync_exclude.cpp

  std/c_time.cpp

)

if (WIN32)
    list(APPEND csync_SRCS
        vio/csync_vio_local_win.cpp
    )
else()
    list(APPEND csync_SRCS
        vio/csync_vio_local_unix.cpp
    )
endif()

configure_file(csync_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/csync_version.h)

add_library(nextcloud_csync SHARED ${common_SOURCES} ${csync_SRCS})

target_include_directories(
  nextcloud_csync
  PUBLIC ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/std
)

if(USE_OUR_OWN_SQLITE3)
  message(STATUS "Using own sqlite3 version")
  add_library(sqlite3 STATIC "${CMAKE_SOURCE_DIR}/src/3rdparty/sqlite3/sqlite3.c")
  target_include_directories(sqlite3 PUBLIC "${CMAKE_SOURCE_DIR}/src/3rdparty/sqlite3")
  target_link_libraries(nextcloud_csync PUBLIC sqlite3)
else()
  target_include_directories(nextcloud_csync PUBLIC ${SQLITE3_INCLUDE_DIR})
  target_link_libraries(nextcloud_csync PUBLIC ${SQLITE3_LIBRARIES})
endif()


generate_export_header(nextcloud_csync
  EXPORT_MACRO_NAME OCSYNC_EXPORT
  EXPORT_FILE_NAME ocsynclib.h
)

target_link_libraries(nextcloud_csync
  PUBLIC
  ${CSYNC_REQUIRED_LIBRARIES}
  Qt5::Core Qt5::Concurrent
)

if(ZLIB_FOUND)
  target_link_libraries(nextcloud_csync PUBLIC ZLIB::ZLIB)
endif(ZLIB_FOUND)


# For src/common/utility_mac.cpp
if (APPLE)
    find_library(FOUNDATION_LIBRARY NAMES Foundation)
    find_library(CORESERVICES_LIBRARY NAMES CoreServices)
    target_link_libraries(nextcloud_csync PUBLIC ${FOUNDATION_LIBRARY} ${CORESERVICES_LIBRARY})
endif()

set_target_properties(
  nextcloud_csync
    PROPERTIES
      VERSION
        ${LIBRARY_VERSION}
      SOVERSION
        ${LIBRARY_SOVERSION}
      RUNTIME_OUTPUT_DIRECTORY
        ${BIN_OUTPUT_DIRECTORY}
      LIBRARY_OUTPUT_NAME
        "${APPLICATION_EXECUTABLE}_csync"
)
if(BUILD_OWNCLOUD_OSX_BUNDLE)
  INSTALL(
    TARGETS
    nextcloud_csync
    LIBRARY DESTINATION
	${LIB_INSTALL_DIR}
    ARCHIVE DESTINATION
	${LIB_INSTALL_DIR}
    RUNTIME DESTINATION
	${BIN_INSTALL_DIR}
    )
else()
  INSTALL(
  TARGETS
  nextcloud_csync
  LIBRARY DESTINATION
    ${CMAKE_INSTALL_LIBDIR}
  ARCHIVE DESTINATION
    ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION
    ${CMAKE_INSTALL_BINDIR}
  )
endif()

configure_file(config_csync.h.cmake ${CMAKE_CURRENT_BINARY_DIR}/config_csync.h)
