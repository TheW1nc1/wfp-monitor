cmake_minimum_required(VERSION 3.20)
project(WfpProject CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 1. User Mode Application ---
add_executable(WfpController WfpController/WfpController.cpp)
target_link_libraries(WfpController PRIVATE Ws2_32)

# --- 2. Kernel Mode Driver ---
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindWDK/cmake")
find_package(WDK REQUIRED)
message(STATUS "Using WDK version: ${WDK_VERSION}")

wdk_add_driver(WfpMonitor WfpMonitor/WfpMonitor.c)

set_source_files_properties(WfpMonitor/WfpMonitor.c PROPERTIES LANGUAGE CXX)

# Link WFP kernel library using FindWDK's generated IMPORTED target (plain style)
target_link_libraries(WfpMonitor WDK::FWPKCLNT)
