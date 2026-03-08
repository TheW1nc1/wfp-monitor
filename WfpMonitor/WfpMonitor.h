#pragma once

// Device name and symbolic link
#define WFP_MONITOR_DEVICE_NAME     L"\\Device\\WfpMonitor"
#define WFP_MONITOR_SYMLINK_KERNEL  L"\\??\\WfpMonitor"
#define WFP_MONITOR_SYMLINK_USER    L"\\\\.\\WfpMonitor"

// IOCTL Definitions
#define WFP_MONITOR_DEVICE_TYPE FILE_DEVICE_UNKNOWN

// IOCTLs
// 1. Set Target PID
#define IOCTL_WFP_MONITOR_SET_PID \
    CTL_CODE(WFP_MONITOR_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_ANY_ACCESS)

// 2. Get Traffic Stats
#define IOCTL_WFP_MONITOR_GET_STATS \
    CTL_CODE(WFP_MONITOR_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)

// Data Structures

// Structure received when setting PID
typedef struct _WFP_MONITOR_SET_PID_IN {
    ULONG ProcessId;
} WFP_MONITOR_SET_PID_IN, *PWFP_MONITOR_SET_PID_IN;

// Structure returned when getting stats
typedef struct _WFP_MONITOR_STATS_OUT {
    ULONG64 TxBytes;
    ULONG64 RxBytes;
    ULONG DestIp;     // IPv4 network byte order
    USHORT DestPort;  // Network byte order
    ULONG DebugCallAle;
    ULONG DebugMatchPid;
    ULONG DebugMatchContext;
} WFP_MONITOR_STATS_OUT, *PWFP_MONITOR_STATS_OUT;
