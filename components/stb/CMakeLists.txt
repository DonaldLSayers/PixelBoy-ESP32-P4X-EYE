idf_component_register(SRCS "stb_write_impl.c" "stb_image_impl.c"
                       INCLUDE_DIRS "include")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function -Wno-missing-field-initializers)
