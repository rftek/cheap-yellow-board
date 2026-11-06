cmake_minimum_required(VERSION 3.16)

# The shared components live in components/ at the repository root.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components/cyd_display")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(stopwatch)
