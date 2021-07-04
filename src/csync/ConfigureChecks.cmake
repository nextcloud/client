include(CheckIncludeFile)
include(CheckSymbolExists)
include(CheckFunctionExists)
include(CheckLibraryExists)
include(CheckTypeSize)
include(CheckCXXSourceCompiles)

set(PACKAGE ${APPLICATION_NAME})
set(VERSION ${APPLICATION_VERSION})
set(LIBDIR ${LIB_INSTALL_DIR})
set(SYSCONFDIR ${SYSCONF_INSTALL_DIR})

set(BINARYDIR ${CMAKE_CURRENT_BINARY_DIR})
set(SOURCEDIR ${CMAKE_CURRENT_SOURCE_DIR})

# HEADER FILES
check_include_file(argp.h HAVE_ARGP_H)

# FUNCTIONS
if (NOT LINUX)
    # librt
    check_library_exists(rt nanosleep "" HAVE_LIBRT)

    set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} )
endif (NOT LINUX)

if(WIN32)
  set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} psapi kernel32)
endif()

check_function_exists(utimes HAVE_UTIMES)
check_function_exists(lstat HAVE_LSTAT)

set(CSYNC_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} CACHE INTERNAL "csync required system libraries")
