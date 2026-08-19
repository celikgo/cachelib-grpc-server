# FindNUMA.cmake - Find the NUMA library
#
# This module defines:
#   NUMA_FOUND       - True if numa is found
#   NUMA_INCLUDE_DIR - The numa include directory
#   NUMA_LIBRARY     - The numa library
#   NUMA::NUMA       - Imported target

include(FindPackageHandleStandardArgs)

# Try pkg-config first
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_NUMA QUIET numa)
endif()

find_path(NUMA_INCLUDE_DIR
  NAMES numa.h
  PATHS ${PC_NUMA_INCLUDE_DIRS}
  PATH_SUFFIXES include
)

find_library(NUMA_LIBRARY
  NAMES numa
  PATHS ${PC_NUMA_LIBRARY_DIRS}
  PATH_SUFFIXES lib lib64
)

find_package_handle_standard_args(NUMA
  REQUIRED_VARS NUMA_LIBRARY NUMA_INCLUDE_DIR
)

if(NUMA_FOUND AND NOT TARGET NUMA::NUMA)
  add_library(NUMA::NUMA UNKNOWN IMPORTED)
  set_target_properties(NUMA::NUMA PROPERTIES
    IMPORTED_LOCATION "${NUMA_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${NUMA_INCLUDE_DIR}"
  )
endif()

mark_as_advanced(NUMA_INCLUDE_DIR NUMA_LIBRARY)
