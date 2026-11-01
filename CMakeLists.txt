cmake_minimum_required(VERSION 3.16)

# Bluepad32 and its BTstack host are vendored as a submodule under external/.
# BTstack's ESP32 port and the Bluepad32 library are each an ESP-IDF component;
# see README.md for how the submodule is initialised and patched in CI.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components"
    "${CMAKE_CURRENT_LIST_DIR}/external/bluepad32/external/btstack/port/esp32/components"
    "${CMAKE_CURRENT_LIST_DIR}/external/bluepad32/src/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xbox_controller)
