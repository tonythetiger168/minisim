# mformal: formal property checking on the msim front end (docs/FORMAL.md).
# Also defines `cadical`, the SAT solver every tool shares (CaDiCaL, MIT,
# pinned to a release).
include_guard(GLOBAL)
if(MINISIM_WITH_SLANG)
  include(FetchContent)
  include(GNUInstallDirs)
  find_package(Threads REQUIRED)
  find_package(Python3 COMPONENTS Interpreter)

  FetchContent_Declare(cadical
    GIT_REPOSITORY https://github.com/arminbiere/cadical.git
    GIT_TAG rel-2.1.3
    GIT_SHALLOW TRUE)
  FetchContent_Populate(cadical)
  file(GLOB CADICAL_SRC ${cadical_SOURCE_DIR}/src/*.cpp)
  list(FILTER CADICAL_SRC EXCLUDE REGEX "/(cadical|mobical)\\.cpp$")
  add_library(cadical STATIC ${CADICAL_SRC})
  target_include_directories(cadical PUBLIC ${cadical_SOURCE_DIR}/src)
  target_compile_definitions(cadical PRIVATE NBUILD NDEBUG NCLOSEFROM)
  target_compile_options(cadical PRIVATE -O3 -w)

  add_executable(mformal ${CMAKE_SOURCE_DIR}/src/mformal.cpp)
  target_include_directories(mformal PRIVATE ${CMAKE_SOURCE_DIR}/src)
  target_link_libraries(mformal PRIVATE slang::slang cadical Threads::Threads)
  install(TARGETS mformal RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

  # Verdicts, thread-count independence, and each counterexample replayed in msim.
  add_test(NAME mformal
           COMMAND ${CMAKE_COMMAND} -DMFORMAL=$<TARGET_FILE:mformal> -DMSIM=$<TARGET_FILE:msim>
                   -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/mformal_test
                   -P ${CMAKE_SOURCE_DIR}/tests/mformal.cmake)
  # mformal against msim on random designs (tests/formal/fuzz.py).
  if(Python3_FOUND)
    add_test(NAME mformal_fuzz
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/formal/fuzz.py ${CMAKE_BINARY_DIR}
                     0 40 ${CMAKE_BINARY_DIR}/mformal_fuzz)
  endif()
endif()
