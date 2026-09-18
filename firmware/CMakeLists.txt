# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Wireless components are intentionally absent from the infrastructure-only build.
# ESP_WIFI_ENABLED is a hidden SoC default in ESP-IDF v6.1, so MINIMAL_BUILD and
# an explicit dependency graph are the effective exclusion mechanism.
idf_build_set_property(MINIMAL_BUILD ON)
project(mc100)
