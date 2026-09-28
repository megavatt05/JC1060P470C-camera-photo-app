# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS
    ./components
    )

add_compile_options(-Wno-maybe-uninitialized)

# ESP-IDF 6.0 + esp_ipa 1.1.x (pulled by esp_video 1.2):
# ensure ISP headers are on the include path when compiling managed esp_ipa.
if(DEFINED ENV{IDF_PATH})
    include_directories("$ENV{IDF_PATH}/components/esp_driver_isp/include")
    include_directories("$ENV{IDF_PATH}/components/hal/include")
    include_directories("$ENV{IDF_PATH}/components/hal/esp32p4/include")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(video_lcd_display)
