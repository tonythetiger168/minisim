# msupport: an AI support agent per product (docs/SUPPORT.md). Python only.
# The test needs no network and no API key; with slang it also runs examples
# on the real msim inside the sandbox.
include_guard(GLOBAL)
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  if(TARGET msim)
    add_test(NAME msupport
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/msupport_test.py ${CMAKE_SOURCE_DIR} $<TARGET_FILE:msim>)
  else()
    add_test(NAME msupport
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/msupport_test.py ${CMAKE_SOURCE_DIR})
  endif()
endif()
