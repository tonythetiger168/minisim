cmake_minimum_required(VERSION 3.20)
project(minisim LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Simulator kernel + hand-built demo design (no external dependencies)
add_executable(minisim src/minisim.cpp)
target_include_directories(minisim PRIVATE src)

# Unit tests: value library, scheduler (test_sched), system tasks (test_sys),
# net initial value (test_net), and the demo VCD's time-0 $dumpvars sample (test_vcd).
enable_testing()
foreach(t test_expr test_z test_sched test_net test_sys)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE src)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
add_executable(test_vcd tests/test_vcd.cpp)
add_test(NAME test_vcd COMMAND test_vcd $<TARGET_FILE:minisim>)

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
