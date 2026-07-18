# Format and lint targets. Manual/CI use — not wired into the build so
# `cmake --build` stays fast. The pre-commit hook is the enforcing gate.

find_program(CLANG_FORMAT_BIN NAMES clang-format)
find_program(CLANG_TIDY_BIN NAMES clang-tidy)

file(GLOB_RECURSE IPC_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/src/*.cpp
  ${CMAKE_SOURCE_DIR}/src/*.h
  ${CMAKE_SOURCE_DIR}/tests/*.cpp
  ${CMAKE_SOURCE_DIR}/tests/*.h)

if(CLANG_FORMAT_BIN)
  add_custom_target(format
    COMMAND ${CLANG_FORMAT_BIN} -i ${IPC_SOURCES}
    COMMENT "clang-format: rewriting sources in place"
    VERBATIM)

  add_custom_target(format-check
    COMMAND ${CLANG_FORMAT_BIN} --dry-run --Werror ${IPC_SOURCES}
    COMMENT "clang-format: checking (no changes)"
    VERBATIM)
endif()

if(CLANG_TIDY_BIN)
  # Pass flags after `--` instead of `-p build`. Homebrew clang-tidy must use
  # its own libc++ headers; reading the compile DB (compiler /usr/bin/c++)
  # makes it search Apple's SDK and fail to find C++23 headers like <print>.
  add_custom_target(tidy
    COMMAND ${CLANG_TIDY_BIN} --warnings-as-errors=* ${IPC_SOURCES}
            -- -x c++ -std=c++${CMAKE_CXX_STANDARD} -I${CMAKE_SOURCE_DIR}/src
    COMMENT "clang-tidy: linting sources"
    VERBATIM)
endif()
