cmake_minimum_required(VERSION 3.20)
project(minisim LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Simulator kernel + hand-built demo design (no external dependencies)
add_executable(minisim src/minisim.cpp)
target_include_directories(minisim PRIVATE src)

# Unit tests for the 4-state value library
enable_testing()
foreach(t test_expr test_z)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE src)
  add_test(NAME ${t} COMMAND ${t})
endforeach()

# Optional: slang front end (downloads and builds slang; takes a while)
option(MINISIM_WITH_SLANG "Build the slang_dump front-end tool" OFF)
if(MINISIM_WITH_SLANG)
  include(FetchContent)
  FetchContent_Declare(slang
    GIT_REPOSITORY https://github.com/MikePopoloski/slang.git
    GIT_TAG master)  # pin a release tag once you have one that works
  FetchContent_MakeAvailable(slang)
  add_executable(slang_dump src/slang_dump.cpp)
  target_link_libraries(slang_dump PRIVATE slang::slang)
endif()
