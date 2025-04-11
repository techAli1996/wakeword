# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

list(APPEND ${EXTRA_COMPONENTS_DIR} components/edge_impulse)

project(keyword)

idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-unused-variable" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-unused-function" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-maybe-uninitialized" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-error=format=" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-unused-but-set-parameter" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-error=nonnull-compare" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-missing-field-initializers" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-deprecated-declarations" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-Wno-format" APPEND)
idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++11" APPEND)


