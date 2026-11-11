# A tool outside the minisim repository, built against the installed
# minisimCore package. tests/core_lib.cmake installs minisim into a scratch
# prefix and configures this project against it, so a kernel header that the
# package forgets to install breaks this build instead of being found through
# the source tree by accident.
cmake_minimum_required(VERSION 3.20)
project(core_consumer LANGUAGES CXX)

# slang is fetched, not installed, by a minisim build, so the driver passes its
# include directories and library here and the package config takes the target
# as given. A consumer with slang installed needs none of this.
if(NOT TARGET slang::slang)
  add_library(slang::slang UNKNOWN IMPORTED)
  set_target_properties(slang::slang PROPERTIES
                        IMPORTED_LOCATION "${SLANG_LIBRARY}"
                        INTERFACE_INCLUDE_DIRECTORIES "${SLANG_INCLUDE_DIRS}")
  if(SLANG_LINK_LIBRARIES)
    set_property(TARGET slang::slang PROPERTY INTERFACE_LINK_LIBRARIES ${SLANG_LINK_LIBRARIES})
  endif()
endif()

find_package(minisimCore REQUIRED)

add_executable(core_consumer main.cpp)
target_link_libraries(core_consumer PRIVATE minisim::core)
