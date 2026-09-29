cmake_minimum_required(VERSION 3.16)

# Shared components (gbcam image core, stb PNG writer) live outside the firmware folder.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../components")

# esp_tinyusb's own tusb_config.h (managed_components/espressif__esp_tinyusb)
# only wires up Kconfig knobs for CDC/MSC/MTP/HID/MIDI/Vendor/ECM/NCM/DFU/BTH -
# it has no video-class support at all, even though the underlying tinyusb
# component (espressif__tinyusb) does. main/app_usb_video_desc.c builds a UVC
# descriptor directly against tinyusb's low-level video types (same approach
# as tinyusb's own examples/device/video_capture, which ships its own
# tusb_config.h with these same three defines), so they're forced on here
# for every component instead - has to happen before project.cmake processes
# components, since tinyusb itself is one of them.
add_compile_definitions(
    CFG_TUD_VIDEO=1
    CFG_TUD_VIDEO_STREAMING=1
    CFG_TUD_VIDEO_STREAMING_EP_BUFSIZE=512)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(gbcam_p4eye)

# C6 co-processor firmware image (see partitions.csv's c6fw entry and the
# README's "Flashing the C6 co-processor") - whatever's in c6fw_image/ gets
# built into a SPIFFS image and flashed to the c6fw partition automatically
# by idf.py flash (FLASH_IN_PROJECT), same as the app/bootloader/partition
# table. Empty by default (nothing to flash the C6 with, harmless) - drop the
# built c6_fw.bin there when the C6's firmware needs (re)flashing.
spiffs_create_partition_image(c6fw c6fw_image FLASH_IN_PROJECT)
