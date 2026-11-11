# slang-tidy's checks inside mlint (src/linttidy.hpp): the check sources of
# the slang tree we already fetch, compiled into mlint. MIT license.
include_guard(GLOBAL)
option(MINISIM_WITH_SLANG_TIDY "Build slang-tidy's checks into mlint (TIDY rules)" ON)
if(MINISIM_WITH_SLANG AND MINISIM_WITH_SLANG_TIDY AND TARGET mlint)
  set(TIDY_DIR ${slang_SOURCE_DIR}/tools/tidy)
  file(GLOB TIDY_SRC ${TIDY_DIR}/src/style/*.cpp ${TIDY_DIR}/src/synthesis/*.cpp)
  list(APPEND TIDY_SRC ${TIDY_DIR}/src/TidyConfig.cpp ${TIDY_DIR}/src/ASTHelperVisitors.cpp)
  add_library(mlint_tidy OBJECT ${TIDY_SRC})
  target_include_directories(mlint_tidy PUBLIC ${TIDY_DIR}/include)
  # fmt and Boost.Regex are the copies slang itself builds with.
  target_link_libraries(mlint_tidy PUBLIC slang::slang fmt::fmt-header-only)
  target_include_directories(mlint_tidy PUBLIC ${boost_regex_SOURCE_DIR}/include)
  target_sources(mlint PRIVATE $<TARGET_OBJECTS:mlint_tidy>)
  target_include_directories(mlint PRIVATE ${TIDY_DIR}/include ${boost_regex_SOURCE_DIR}/include)
  target_link_libraries(mlint PRIVATE fmt::fmt-header-only)
  target_compile_definitions(mlint PRIVATE MINISIM_TIDY)
endif()
