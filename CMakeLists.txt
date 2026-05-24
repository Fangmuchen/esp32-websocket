cmake_minimum_required(VERSION 3.5)

# 如果 IDF_PATH 环境变量未设置或指向 Windows 路径，回退到本地路径
if(NOT DEFINED ENV{IDF_PATH} OR "$ENV{IDF_PATH}" MATCHES "^[A-Z]:")
    set(ENV{IDF_PATH} "/home/lucas/esp/v5.4.2/esp-idf")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_pooltable)
