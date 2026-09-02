#----------------------------------------------------------------
# Generated CMake target import file for configuration "None".
#----------------------------------------------------------------

# Commands may need to know the format version.
set(CMAKE_IMPORT_FILE_VERSION 1)

# Import target "phi::runtime" for configuration "None"
set_property(TARGET phi::runtime APPEND PROPERTY IMPORTED_CONFIGURATIONS NONE)
set_target_properties(phi::runtime PROPERTIES
  IMPORTED_LOCATION_NONE "${_IMPORT_PREFIX}/lib/aarch64-linux-gnu/libphi_runtime.so.0.1.0"
  IMPORTED_SONAME_NONE "libphi_runtime.so.0"
  )

list(APPEND _cmake_import_check_targets phi::runtime )
list(APPEND _cmake_import_check_files_for_phi::runtime "${_IMPORT_PREFIX}/lib/aarch64-linux-gnu/libphi_runtime.so.0.1.0" )

# Commands beyond this point should not need to know the version.
set(CMAKE_IMPORT_FILE_VERSION)
