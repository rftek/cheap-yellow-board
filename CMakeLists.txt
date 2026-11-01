cmake_minimum_required(VERSION 3.16)

# Bluepad32 + BTstack are vendored under external/bluepad32 (BTstack nested at
# external/btstack). BLUEPAD32_ROOT and BTSTACK_ROOT are exported by the CI
# workflow and must be set for a local build too; see README.md. BTstack is
# turned into an ESP-IDF component by the CMake file included below (the same
# way Bluepad32's own esp32 example does it), not by a separate script.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/components"
    "$ENV{BLUEPAD32_ROOT}/src/components")

include($ENV{BTSTACK_ROOT}/port/esp32/components/btstack/cmake/BTstack_project.cmake)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xbox_controller)
