# 固定上游 v2.3.0，只构建读取器。
idf_component_register(INCLUDE_DIRS "upstream/core/src" PRIV_REQUIRES pthread barcode_runtime)
if(CMAKE_BUILD_EARLY_EXPANSION)
    return()
endif()
set(ZXING_READERS ON)
set(ZXING_WRITERS OFF)
set(ZXING_C_API OFF)
set(BUILD_SHARED_LIBS OFF)
add_subdirectory(upstream/core)
set_target_properties(ZXing PROPERTIES DISABLE_PRECOMPILE_HEADERS ON POSITION_INDEPENDENT_CODE OFF)
target_compile_options(ZXing PRIVATE -O2 -Wno-error=type-limits $<$<COMPILE_LANGUAGE:CXX>:-fexceptions;-frtti> $<$<COMPILE_LANGUAGE:C>:-Wno-error=undef>)
target_link_libraries(${COMPONENT_LIB} INTERFACE ZXing)

# 上游目标使用本地协作调度钩子，非 ESP 主机构建保持原样。
target_include_directories(ZXing PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../barcode_runtime/include")
