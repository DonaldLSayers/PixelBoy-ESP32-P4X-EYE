# ESP-IDF component. The host tool (tools/host) compiles these sources directly.
idf_component_register(SRCS "src/gbcam.c" "src/gbcam_dither.c" "src/gbcam_palette.c" "src/gbcam_pixelcam.c" "src/dithercam.c"
                       INCLUDE_DIRS "include")
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
