cmake_minimum_required(VERSION 3.20)

project(ipc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd/IDE tooling resolves includes and flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(ipc src/main.cpp)

include(cmake/CodeQuality.cmake)
include(cmake/GitHooks.cmake)
