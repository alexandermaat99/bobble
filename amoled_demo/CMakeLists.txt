cmake_minimum_required(VERSION 3.16)
# Each build folder keeps its own settings, made from sdkconfig.defaults the first time: never a stale sdkconfig
# beside this file (one from the round board's days ran the Square with settings that broke its picture)
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(amoled_demo)
