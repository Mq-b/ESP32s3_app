idf_component_register(SRCS "stb_image_impl.c" INCLUDE_DIRS "include" PRIV_REQUIRES heap barcode_runtime)
target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -Wno-unused-function)
