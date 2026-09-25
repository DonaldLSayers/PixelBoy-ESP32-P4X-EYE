cmake_minimum_required(VERSION 3.16)

# Shared components (gbcam image core, stb PNG writer) live outside the firmware folder.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gbcam_p4eye)
