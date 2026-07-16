# Enable the repository pre-commit hook on the first CMake configure, so a
# fresh clone gets the format/lint gate without a manual setup step.
if(EXISTS ${CMAKE_SOURCE_DIR}/.git)
  execute_process(
    COMMAND git config core.hooksPath .githooks
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
endif()
