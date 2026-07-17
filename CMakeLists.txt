cmake_minimum_required(VERSION 3.20)

project(ipc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd/IDE tooling resolves includes and flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CompilerWarnings.cmake)

add_executable(ipc src/main.cpp)
ipc_enable_warnings(ipc)

add_library(producer_lib
  src/producer/command_line_parser.cpp
)
target_include_directories(producer_lib PUBLIC src)
ipc_enable_warnings(producer_lib)

include(cmake/CodeQuality.cmake)
include(cmake/GitHooks.cmake)
