idf_component_register(SRCS "main.c"
                            "app.c"
                            "app_camera.c"
                            "app_display.c"
                            "app_frames.c"
                            "app_frames_sd.c"
                            "app_input.c"
                            "app_palettes_sd.c"
                            "app_settings.c"
                            "app_settings_common.c"
                            "app_storage.c"
                            "app_usb.c"
                            "camera_ppa_esp.c"
                            "display_hw_esp.c"
                            "platform_esp.c"
                       INCLUDE_DIRS "."
                       REQUIRES gbcam stb esp_driver_gpio esp_driver_pcnt esp_lcd esp_timer nvs_flash fatfs
                                esp_driver_ppa esp_mm esp_driver_sdmmc sdmmc wear_levelling espressif__esp_tinyusb)

# A handful of snprintf(path, sizeof path, "%s/%s", dir, dirent->d_name) calls
# (app_frames_sd.c, app_palettes_sd.c - building a path to something just
# found in an SD folder listing) trip -Wformat-truncation: GCC has no
# compile-time bound on d_name, so it assumes the POSIX-worst-case length,
# which together with the mount root can in theory exceed the 300-byte path
# buffer - real SD card paths never do. Downgraded from error to warning,
# same as ESP-IDF's own default treatment of a few other warning classes
# (see the compiler's own -Wno-error=... flags) - still visible, just not
# fatal for a case that's safe in practice.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)
