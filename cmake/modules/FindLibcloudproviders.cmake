# FindLibcloudproviders.cmake

find_path(LIBCLOUDPROVIDERS_INCLUDE_DIR
    NAMES cloudprovidersproviderexporter.h cloudprovidersaccountexporter.h
    PATH_SUFFIXES cloudproviders
)
find_library(LIBCLOUDPROVIDERS_LIBRARY
    NAMES
        libcloudproviders
        cloudproviders
    HINTS
       /usr/lib
       /usr/lib/${CMAKE_ARCH_TRIPLET}
       /usr/local/lib
       /opt/local/lib
       ${CMAKE_LIBRARY_PATH}
       ${CMAKE_INSTALL_PREFIX}/lib
)

include(FindPackageHandleStandardArgs)

find_package_handle_standard_args(LIBCLOUDPROVIDERS DEFAULT_MSG LIBCLOUDPROVIDERS_LIBRARY LIBCLOUDPROVIDERS_INCLUDE_DIR)
