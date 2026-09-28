cmake_minimum_required(VERSION 3.16)

# Shared components (gbcam image core, stb PNG writer) live outside the firmware folder.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gbcam_p4eye)

# C6 co-processor firmware image (see partitions.csv's c6fw entry and the
# README's "Flashing the C6 co-processor") - whatever's in c6fw_image/ gets
# built into a SPIFFS image and flashed to the c6fw partition automatically
# by idf.py flash (FLASH_IN_PROJECT), same as the app/bootloader/partition
# table. Empty by default (nothing to flash the C6 with, harmless) - drop the
# built c6_fw.bin there when the C6's firmware needs (re)flashing.
spiffs_create_partition_image(c6fw c6fw_image FLASH_IN_PROJECT)
