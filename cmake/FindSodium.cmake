# FindSodium.cmake - Find the libsodium library
#
# This module defines:
#   Sodium_FOUND       - True if libsodium is found
#   Sodium_INCLUDE_DIR - The libsodium include directory
#   Sodium_LIBRARY     - The libsodium library
#   Sodium::sodium     - Imported target

include(FindPackageHandleStandardArgs)

# Try pkg-config first
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_SODIUM QUIET libsodium)
endif()

find_path(Sodium_INCLUDE_DIR
  NAMES sodium.h
  PATHS ${PC_SODIUM_INCLUDE_DIRS}
  PATH_SUFFIXES include
)

find_library(Sodium_LIBRARY
  NAMES sodium libsodium
  PATHS ${PC_SODIUM_LIBRARY_DIRS}
  PATH_SUFFIXES lib lib64
)

find_package_handle_standard_args(Sodium
  REQUIRED_VARS Sodium_LIBRARY Sodium_INCLUDE_DIR
)

if(Sodium_FOUND AND NOT TARGET Sodium::sodium)
  add_library(Sodium::sodium UNKNOWN IMPORTED)
  set_target_properties(Sodium::sodium PROPERTIES
    IMPORTED_LOCATION "${Sodium_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${Sodium_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(Sodium_INCLUDE_DIR Sodium_LIBRARY)
