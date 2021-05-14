project(tests)

set(TORTURE_LIBRARY torture)

include_directories(
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_BINARY_DIR}
  ${CMOCKA_INCLUDE_DIR}
  ${CHECK_INCLUDE_DIRS}
  ${CMAKE_BINARY_DIR}/src/csync
)

# create test library
add_library(${TORTURE_LIBRARY} STATIC torture.c cmdline.c)
target_link_libraries(${TORTURE_LIBRARY} ${CMOCKA_LIBRARIES})

set(TEST_TARGET_LIBRARIES ${TORTURE_LIBRARY} Qt5::Core nextcloud_csync)

# create tests

# std

add_cmocka_test(check_std_c_jhash std_tests/check_std_c_jhash.c ${TEST_TARGET_LIBRARIES})

# vio
add_cmocka_test(check_vio_ext vio_tests/check_vio_ext.cpp ${TEST_TARGET_LIBRARIES})
