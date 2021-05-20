# SPDX-FileCopyrightText: 2014 Aleix Pol Gonzalez <aleixpol@kde.org>
#
# SPDX-License-Identifier: BSD-3-Clause

#[=======================================================================[.rst:
ECMCoverageOption
--------------------

Allow users to easily enable GCov code coverage support.

Code coverage allows you to check how much of your codebase is covered by
your tests. This module makes it easy to build with support for
`GCov <https://gcc.gnu.org/onlinedocs/gcc/Gcov.html>`_.

When this module is included, a ``BUILD_COVERAGE`` option is added (default
OFF). Turning this option on enables GCC's coverage instrumentation, and
links against ``libgcov``.

Note that this will probably break the build if you are not using GCC.

Since 1.3.0.
#]=======================================================================]

option(BUILD_COVERAGE "Build the project with gcov support" OFF)

if(BUILD_COVERAGE)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-arcs -ftest-coverage")
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -lgcov")
endif()
