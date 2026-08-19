# FindXxhash.cmake - Find the xxhash library
#
# This module defines:
#   Xxhash_FOUND       - True if xxhash is found
#   Xxhash_INCLUDE_DIR - The xxhash include directory
#   Xxhash_LIBRARY     - The xxhash library
#   Xxhash::xxhash     - Imported target

include(FindPackageHandleStandardArgs)

# Try pkg-config first
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_XXHASH QUIET libxxhash)
endif()

# Search in CMAKE_PREFIX_PATH directories
find_path(Xxhash_INCLUDE_DIR
  NAMES xxhash.h
  PATHS ${PC_XXHASH_INCLUDE_DIRS}
  PATH_SUFFIXES include
)

find_library(Xxhash_LIBRARY
  NAMES xxhash libxxhash
  PATHS ${PC_XXHASH_LIBRARY_DIRS}
  PATH_SUFFIXES lib lib64
)

find_package_handle_standard_args(Xxhash
  REQUIRED_VARS Xxhash_LIBRARY Xxhash_INCLUDE_DIR
)

if(Xxhash_FOUND AND NOT TARGET Xxhash::xxhash)
  add_library(Xxhash::xxhash UNKNOWN IMPORTED)
  set_target_properties(Xxhash::xxhash PROPERTIES
    IMPORTED_LOCATION "${Xxhash_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${Xxhash_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(Xxhash_INCLUDE_DIR Xxhash_LIBRARY)
