cmake_minimum_required(VERSION 3.16)

# Bluepad32 + BTstack are vendored under external/bluepad32 (BTstack nested at
# external/btstack), the same wiring as xbox_controller. BLUEPAD32_ROOT
# and BTSTACK_ROOT are exported by the CI workflow and must be set for a local
# build; see README.md. BTstack_project.cmake must be included after project.cmake
# (it uses idf_build_get_property) and before project() reads the component dirs.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/cyd_display"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/cyd_gamepad"
    "$ENV{BLUEPAD32_ROOT}/src/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
include($ENV{BTSTACK_ROOT}/port/esp32/components/btstack/cmake/BTstack_project.cmake)
project(catmouse_pad)
