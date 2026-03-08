cmake_minimum_required(VERSION 3.20)
project(WfpProject CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 1. User Mode Application ---
add_executable(WfpController WfpController/WfpController.cpp)
target_link_libraries(WfpController PRIVATE Ws2_32)

# --- 2. Kernel Mode Driver ---
# 使用 FindWDK 来编译内核驱动
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindWDK/cmake")
find_package(WDK REQUIRED)

wdk_add_driver(WfpMonitor WfpMonitor/WfpMonitor.c)

# 链接 WFP 需要的内核库。
# FindWDK 为 WDK Lib 目录中的所有 .lib 生成了 WDK::LIBNAME 格式的导入目标。
# 使用 plain 风格 (不带 PRIVATE 关键字) 以兼容 FindWdk.cmake 内部的 plain风格调用。
target_link_libraries(WfpMonitor WDK::FWPKCLNT)
