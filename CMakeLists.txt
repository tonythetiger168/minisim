cmake_minimum_required(VERSION 3.20)
project(minisim LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Simulator kernel + hand-built demo design (no external dependencies)
add_executable(minisim src/minisim.cpp)
target_include_directories(minisim PRIVATE src)

# Unit tests for the 4-state value library
enable_testing()
foreach(t test_expr test_z test_mux test_coverage test_properties test_callbacks)
  add_executable(${t} tests/${t}.cpp)
  target_include_directories(${t} PRIVATE src)
  # Existing tests use assert; keep checks enabled in Release builds too.
  if(MSVC)
    target_compile_options(${t} PRIVATE /UNDEBUG)
  else()
    target_compile_options(${t} PRIVATE -UNDEBUG)
  endif()
  add_test(NAME ${t} COMMAND ${t})
endforeach()
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-output/coverage")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-output/callbacks")
set_tests_properties(test_callbacks PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-output/callbacks" TIMEOUT 10)
set_tests_properties(test_coverage PROPERTIES
  WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/test-output/coverage" TIMEOUT 10)

add_executable(test_kernel tests/test_kernel.cpp)
target_include_directories(test_kernel PRIVATE src)
foreach(scenario nba multi_wait edges finish_active finish_nba finish_ir vcd_initial resume_budget resume_budget_time ir_budget ir_budget_wait process_exception delay_overflow delay_boundary)
  set(output_dir "${CMAKE_CURRENT_BINARY_DIR}/test-output/${scenario}")
  file(MAKE_DIRECTORY "${output_dir}")
  add_test(NAME kernel_${scenario} COMMAND test_kernel ${scenario})
  set_tests_properties(kernel_${scenario} PROPERTIES WORKING_DIRECTORY "${output_dir}" TIMEOUT 10)
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
