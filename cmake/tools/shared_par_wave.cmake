# Shared by the tools: the thread pool and -threads N (src/parallel.hpp) and
# the VCD / FST waveform reader (src/wave_reader.hpp). Compiler only.
include_guard(GLOBAL)
find_package(Threads REQUIRED)
foreach(t test_parallel test_wave)
  add_executable(${t} ${CMAKE_SOURCE_DIR}/tests/${t}.cpp)
  target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_compile_options(${t} PRIVATE -UNDEBUG)   # asserts stay on in Release too
  target_link_libraries(${t} PRIVATE Threads::Threads)
  add_test(NAME ${t} COMMAND ${t})
endforeach()
