# Vendor command-line compatibility layer (src/compat.hpp), shared by every
# tool. Header only; this file builds its unit test. Compiler only, no slang.
include_guard(GLOBAL)
add_executable(test_compat ${CMAKE_SOURCE_DIR}/tests/test_compat.cpp)
target_include_directories(test_compat PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_compile_options(test_compat PRIVATE -UNDEBUG)   # asserts stay on in Release too
add_test(NAME test_compat COMMAND test_compat)
