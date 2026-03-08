cmake_minimum_required(VERSION 3.20)
project(WfpProject CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# --- 1. User Mode Application ---
add_executable(WfpController WfpController/WfpController.cpp)
target_link_libraries(WfpController PRIVATE Ws2_32)

# --- 2. Kernel Mode Driver ---
# WDK version selection strategy:
# FindWDK picks the LATEST installed WDK by default, but WDK 26100 has a known
# IF_INDEX redefinition bug in fwpsk.h. We prefer older stable versions.
# Try 22621 first, then 22000, then fall back to whatever FindWDK discovers.
foreach(_wdk_ver 10.0.22621.0 10.0.22000.0 10.0.19041.0)
    set(_wdk_probe "C:/Program Files (x86)/Windows Kits/10/Include/${_wdk_ver}/km/ntddk.h")
    if(EXISTS "${_wdk_probe}" AND NOT DEFINED WDK_LATEST_NTDDK_FILE_FORCED)
        message(STATUS "Forcing WDK ${_wdk_ver} (avoids fwpsk.h IF_INDEX bug in 26100)")
        set(WDK_LATEST_NTDDK_FILE "${_wdk_probe}" CACHE FILEPATH "" FORCE)
        set(WDK_LATEST_NTDDK_FILE_FORCED TRUE CACHE BOOL "" FORCE)
    endif()
endforeach()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake/FindWDK/cmake")
find_package(WDK REQUIRED)
message(STATUS "Using WDK version: ${WDK_VERSION}")

wdk_add_driver(WfpMonitor WfpMonitor/WfpMonitor.c)

# Force C++ compilation for WfpMonitor.c to work around WDK 26100 fwpsk.h bug.
# In C mode, repeated typedef of 'IF_INDEX' is illegal (C2370).
# In C++ mode, identical typedef redefinitions are allowed.
set_source_files_properties(WfpMonitor/WfpMonitor.c PROPERTIES LANGUAGE CXX)

# Link WFP kernel library using FindWDK's generated IMPORTED target (plain style)
target_link_libraries(WfpMonitor WDK::FWPKCLNT)
