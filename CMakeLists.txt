cmake_minimum_required(VERSION 3.20)

project(ipc LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Emit compile_commands.json so clangd/IDE tooling resolves includes and flags.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CompilerWarnings.cmake)

find_package(Threads REQUIRED)

add_library(common_lib
  src/common/transport/control_block.cpp
  src/common/transport/mapped_segment.cpp
  src/common/transport/shared_memory_transport.cpp
  src/common/util/cli.cpp
)
target_include_directories(common_lib PUBLIC src)
target_link_libraries(common_lib PUBLIC Threads::Threads)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_libraries(common_lib PUBLIC rt)
endif()
ipc_enable_warnings(common_lib)

add_library(producer_lib
  src/producer/app.cpp
  src/producer/command_line_parser.cpp
)
target_include_directories(producer_lib PUBLIC src)
target_link_libraries(producer_lib PUBLIC common_lib)
ipc_enable_warnings(producer_lib)

add_executable(producer-cli src/producer/main.cpp)
target_link_libraries(producer-cli PRIVATE producer_lib)
ipc_enable_warnings(producer-cli)

add_library(consumer_lib
  src/consumer/app.cpp
  src/consumer/command_line_parser.cpp
)
target_include_directories(consumer_lib PUBLIC src)
target_link_libraries(consumer_lib PUBLIC common_lib)
ipc_enable_warnings(consumer_lib)

add_executable(consumer-cli src/consumer/main.cpp)
target_link_libraries(consumer-cli PRIVATE consumer_lib)
ipc_enable_warnings(consumer-cli)

include(cmake/CodeQuality.cmake)
include(cmake/GitHooks.cmake)
