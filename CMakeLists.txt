cmake_minimum_required(VERSION 3.20)
project(WfpProject CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 1. User Mode Application ---
add_executable(WfpController WfpController/WfpController.cpp)
target_link_libraries(WfpController PRIVATE Ws2_32)

# --- 2. Kernel Mode Driver ---
# Force WDK 22621 to avoid fwpsk.h IF_INDEX redefinition bug in WDK 26100.
# FindWDK by default picks the LATEST installed WDK version (26100 > 22621).
# By setting WDK_LATEST_NTDDK_FILE explicitly, we pin to 22621.
set(WDK_22621_PATH "C:/Program Files (x86)/Windows Kits/10/Include/10.0.22621.0/km/ntddk.h")
if(EXISTS "${WDK_22621_PATH}")
    message(STATUS "Forcing WDK 22621 to avoid fwpsk.h IF_INDEX conflict in 26100")
    set(WDK_LATEST_NTDDK_FILE "${WDK_22621_PATH}" CACHE FILEPATH "" FORCE)
else()
    message(WARNING "WDK 22621 not found at expected path, FindWDK will use latest (may cause IF_INDEX conflict)")
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindWDK/cmake")
find_package(WDK REQUIRED)

wdk_add_driver(WfpMonitor WfpMonitor/WfpMonitor.c)

# Link WFP kernel library using FindWDK's IMPORTED target (plain style, not PRIVATE keyword)
target_link_libraries(WfpMonitor WDK::FWPKCLNT)
