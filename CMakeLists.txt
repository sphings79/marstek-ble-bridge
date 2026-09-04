cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(marstek-ble-bridge)

# The web app bundle is flashed alongside the firmware as its own partition, so a single
# web-flash click delivers both. web/ is populated from the marstek-ble-control build.
spiffs_create_partition_image(web ${CMAKE_CURRENT_SOURCE_DIR}/web FLASH_IN_PROJECT)
