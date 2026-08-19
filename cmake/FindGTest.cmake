# FindGTest.cmake - Find GTest library
#
# On Ubuntu, GTest is typically at /usr/lib/aarch64-linux-gnu/cmake/GTest/
# This module provides a fallback if the system GTest is not found via CONFIG mode

include(FindPackageHandleStandardArgs)

# First try to find GTest in config mode (from Ubuntu's libgtest-dev)
find_package(GTest CONFIG QUIET)

if(GTest_FOUND)
  message(STATUS "Found GTest via CONFIG mode")
  return()
endif()

# Try to find via pkg-config
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_GTEST QUIET gtest)
endif()

# Find the headers
find_path(GTEST_INCLUDE_DIR
  NAMES gtest/gtest.h
  PATHS ${PC_GTEST_INCLUDE_DIRS}
  PATH_SUFFIXES include
)

# Find the library
find_library(GTEST_LIBRARY
  NAMES gtest
  PATHS ${PC_GTEST_LIBRARY_DIRS}
  PATH_SUFFIXES lib lib64
)

find_library(GTEST_MAIN_LIBRARY
  NAMES gtest_main
  PATHS ${PC_GTEST_LIBRARY_DIRS}
  PATH_SUFFIXES lib lib64
)

find_package_handle_standard_args(GTest
  REQUIRED_VARS GTEST_LIBRARY GTEST_INCLUDE_DIR
)

if(GTest_FOUND)
  if(NOT TARGET GTest::gtest)
    add_library(GTest::gtest UNKNOWN IMPORTED)
    set_target_properties(GTest::gtest PROPERTIES
      IMPORTED_LOCATION "${GTEST_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${GTEST_INCLUDE_DIR}"
    )
  endif()

  if(GTEST_MAIN_LIBRARY AND NOT TARGET GTest::gtest_main)
    add_library(GTest::gtest_main UNKNOWN IMPORTED)
    set_target_properties(GTest::gtest_main PROPERTIES
      IMPORTED_LOCATION "${GTEST_MAIN_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${GTEST_INCLUDE_DIR}"
    )
  endif()

  set(GTest_FOUND TRUE)
endif()

mark_as_advanced(GTEST_INCLUDE_DIR GTEST_LIBRARY GTEST_MAIN_LIBRARY)
