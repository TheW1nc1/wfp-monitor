#include <ntddk.h>
#pragma warning(push)
#pragma warning(disable:4201)       // unnamed struct/union

#define NDIS_MINIPORT_DRIVER 1
#define NDIS60_MINIPORT 1           // Target NDIS 6.0+ for Windows Vista and later
#include <ndis.h>           // Required for NDIS_HANDLE used in fwpsk.h
#include <fwpsk.h>
#pragma warning(pop)
#include <fwpmk.h>

#include "WfpMonitor.h"

//
// Globals
//
PDEVICE_OBJECT g_DeviceObject = NULL;
HANDLE g_EngineHandle = NULL;
UINT32 g_CalloutIdAleFlowV4 = 0;
UINT32 g_CalloutIdStreamV4 = 0;
UINT32 g_CalloutIdAleFlowV6 = 0;
UINT32 g_CalloutIdStreamV6 = 0;
UINT64 g_FilterIdAleFlowV4 = 0;
UINT64 g_FilterIdStreamV4 = 0;
UINT64 g_FilterIdAleFlowV6 = 0;
UINT64 g_FilterIdStreamV6 = 0;

ULONG g_TargetPid = 0;
WFP_MONITOR_STATS_OUT g_Stats = { 0 };
KSPIN_LOCK g_StatsLock;

// Custom Context Tag
#define WFP_CONTEXT_TAG 'PFWM'
UINT64 g_FlowContextValue = 0xDEADBEEFCAFE;

//
// Function Prototypes
//
DRIVER_INITIALIZE DriverEntry;
DRIVER_UNLOAD WfpMonitorUnload;
NTSTATUS RegisterCallouts(DEVICE_OBJECT* deviceObject);
void UnregisterCallouts();
NTSTATUS WfpMonitorCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp);
NTSTATUS WfpMonitorDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp);

// Callout Functions
void AleFlowEstablishedClassify(
    const FWPS_INCOMING_VALUES0* inFixedValues,
    const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    void* layerData,
    const void* classifyContext,
    const FWPS_FILTER0* filter,
    UINT64 flowContext,
    FWPS_CLASSIFY_OUT0* classifyOut
);
NTSTATUS AleFlowEstablishedNotify(FWPS_CALLOUT_NOTIFY_TYPE notifyType, const GUID* filterKey, FWPS_FILTER0* filter);

void StreamClassify(
    const FWPS_INCOMING_VALUES0* inFixedValues,
    const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    void* layerData,
    const void* classifyContext,
    const FWPS_FILTER0* filter,
    UINT64 flowContext,
    FWPS_CLASSIFY_OUT0* classifyOut
);
NTSTATUS StreamNotify(FWPS_CALLOUT_NOTIFY_TYPE notifyType, const GUID* filterKey, FWPS_FILTER0* filter);
void StreamFlowDelete(UINT16 layerId, UINT32 calloutId, UINT64 flowContext);

// Device Control Definitions
#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, WfpMonitorUnload)
#pragma alloc_text (PAGE, WfpMonitorCreateClose)
#pragma alloc_text (PAGE, WfpMonitorDeviceControl)
#endif


NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    NTSTATUS status;
    UNICODE_STRING deviceName;
    UNICODE_STRING symLinkName;

    UNREFERENCED_PARAMETER(RegistryPath);

    RtlInitUnicodeString(&deviceName, WFP_MONITOR_DEVICE_NAME);
    RtlInitUnicodeString(&symLinkName, WFP_MONITOR_SYMLINK_KERNEL);

    // Create Device
    status = IoCreateDevice(
        DriverObject,
        0,
        &deviceName,
        WFP_MONITOR_DEVICE_TYPE,
        0,
        FALSE,
        &g_DeviceObject
    );

    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = IoCreateSymbolicLink(&symLinkName, &deviceName);
    if (!NT_SUCCESS(status)) {
        IoDeleteDevice(g_DeviceObject);
        return status;
    }

    DriverObject->DriverUnload = WfpMonitorUnload;
    DriverObject->MajorFunction[IRP_MJ_CREATE] = WfpMonitorCreateClose;
    DriverObject->MajorFunction[IRP_MJ_CLOSE] = WfpMonitorCreateClose;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = WfpMonitorDeviceControl;

    KeInitializeSpinLock(&g_StatsLock);

    // Open WFP Engine
    FWPM_SESSION0 session = { 0 };
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;
    status = FwpmEngineOpen0(NULL, RPC_C_AUTHN_WINNT, NULL, &session, &g_EngineHandle);
    if (!NT_SUCCESS(status)) {
        IoDeleteSymbolicLink(&symLinkName);
        IoDeleteDevice(g_DeviceObject);
        return status;
    }

    status = RegisterCallouts(g_DeviceObject);
    if (!NT_SUCCESS(status)) {
        FwpmEngineClose0(g_EngineHandle);
        IoDeleteSymbolicLink(&symLinkName);
        IoDeleteDevice(g_DeviceObject);
    }

    return status;
}

void WfpMonitorUnload(PDRIVER_OBJECT DriverObject)
{
    UNICODE_STRING symLinkName;
    UNREFERENCED_PARAMETER(DriverObject);
    
    UnregisterCallouts();

    if (g_EngineHandle != NULL) {
        FwpmEngineClose0(g_EngineHandle);
        g_EngineHandle = NULL;
    }

    RtlInitUnicodeString(&symLinkName, WFP_MONITOR_SYMLINK_KERNEL);
    IoDeleteSymbolicLink(&symLinkName);

    if (g_DeviceObject != NULL) {
        IoDeleteDevice(g_DeviceObject);
        g_DeviceObject = NULL;
    }
}

NTSTATUS WfpMonitorCreateClose(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    Irp->IoStatus.Status = STATUS_SUCCESS;
    Irp->IoStatus.Information = 0;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return STATUS_SUCCESS;
}

NTSTATUS WfpMonitorDeviceControl(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
    UNREFERENCED_PARAMETER(DeviceObject);
    PIO_STACK_LOCATION irpSp = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS status = STATUS_SUCCESS;
    ULONG info = 0;

    switch (irpSp->Parameters.DeviceIoControl.IoControlCode)
    {
    case IOCTL_WFP_MONITOR_SET_PID:
    {
        if (irpSp->Parameters.DeviceIoControl.InputBufferLength >= sizeof(WFP_MONITOR_SET_PID_IN))
        {
            PWFP_MONITOR_SET_PID_IN inBuf = (PWFP_MONITOR_SET_PID_IN)Irp->AssociatedIrp.SystemBuffer;
            
            KIRQL oldIrql;
            KeAcquireSpinLock(&g_StatsLock, &oldIrql);
            g_TargetPid = inBuf->ProcessId;
            RtlZeroMemory(&g_Stats, sizeof(g_Stats));
            KeReleaseSpinLock(&g_StatsLock, oldIrql);
            
            info = sizeof(WFP_MONITOR_SET_PID_IN);
        }
        else
        {
            status = STATUS_INVALID_PARAMETER;
        }
        break;
    }
    case IOCTL_WFP_MONITOR_GET_STATS:
    {
        if (irpSp->Parameters.DeviceIoControl.OutputBufferLength >= sizeof(WFP_MONITOR_STATS_OUT))
        {
            PWFP_MONITOR_STATS_OUT outBuf = (PWFP_MONITOR_STATS_OUT)Irp->AssociatedIrp.SystemBuffer;
            
            KIRQL oldIrql;
            KeAcquireSpinLock(&g_StatsLock, &oldIrql);
            *outBuf = g_Stats;
            KeReleaseSpinLock(&g_StatsLock, oldIrql);
            
            info = sizeof(WFP_MONITOR_STATS_OUT);
        }
        else
        {
            status = STATUS_INVALID_PARAMETER;
        }
        break;
    }
    default:
        status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return status;
}

// GUIDs
#include <initguid.h>
// {11111111-2222-3333-4444-555555555555}
DEFINE_GUID(WFP_MONITOR_CALLOUT_ALE_FLOW_V4,
    0x11111111, 0x2222, 0x3333, 0x44, 0x44, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55);
// {66666666-7777-8888-9999-000000000000}
DEFINE_GUID(WFP_MONITOR_CALLOUT_STREAM_V4,
    0x66666666, 0x7777, 0x8888, 0x99, 0x99, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00);
// {A1A1A1A1-B2B2-C3C3-D4D4-E5E5E5E5E5E5}
DEFINE_GUID(WFP_MONITOR_CALLOUT_ALE_FLOW_V6,
    0xa1a1a1a1, 0xb2b2, 0xc3c3, 0xd4, 0xd4, 0xe5, 0xe5, 0xe5, 0xe5, 0xe5, 0xe5);
// {F6F6F6F6-A7A7-B8B8-C9C9-D0D0D0D0D0D0}
DEFINE_GUID(WFP_MONITOR_CALLOUT_STREAM_V6,
    0xf6f6f6f6, 0xa7a7, 0xb8b8, 0xc9, 0xc9, 0xd0, 0xd0, 0xd0, 0xd0, 0xd0, 0xd0);

NTSTATUS RegisterCallouts(DEVICE_OBJECT* deviceObject)
{
    NTSTATUS status;

    // 1. ALE Flow Established Callout structure V4
    FWPS_CALLOUT0 calloutAle = { 0 };
    calloutAle.calloutKey = WFP_MONITOR_CALLOUT_ALE_FLOW_V4;
    calloutAle.classifyFn = (FWPS_CALLOUT_CLASSIFY_FN0)AleFlowEstablishedClassify;
    calloutAle.notifyFn = (FWPS_CALLOUT_NOTIFY_FN0)AleFlowEstablishedNotify;
    calloutAle.flowDeleteFn = NULL;

    status = FwpsCalloutRegister0(deviceObject, &calloutAle, &g_CalloutIdAleFlowV4);
    if (!NT_SUCCESS(status)) return status;

    // Stream callout V4
    FWPS_CALLOUT0 calloutStream = { 0 };
    calloutStream.calloutKey = WFP_MONITOR_CALLOUT_STREAM_V4;
    calloutStream.classifyFn = (FWPS_CALLOUT_CLASSIFY_FN0)StreamClassify;
    calloutStream.notifyFn = (FWPS_CALLOUT_NOTIFY_FN0)StreamNotify;
    calloutStream.flowDeleteFn = (FWPS_CALLOUT_FLOW_DELETE_NOTIFY_FN0)StreamFlowDelete;
    status = FwpsCalloutRegister0(deviceObject, &calloutStream, &g_CalloutIdStreamV4);
    if (!NT_SUCCESS(status)) return status;

    // ALE Flow Established callout V6
    FWPS_CALLOUT0 calloutAleV6 = { 0 };
    calloutAleV6.calloutKey = WFP_MONITOR_CALLOUT_ALE_FLOW_V6;
    calloutAleV6.classifyFn = (FWPS_CALLOUT_CLASSIFY_FN0)AleFlowEstablishedClassify;
    calloutAleV6.notifyFn = (FWPS_CALLOUT_NOTIFY_FN0)AleFlowEstablishedNotify;
    calloutAleV6.flowDeleteFn = NULL;
    status = FwpsCalloutRegister0(deviceObject, &calloutAleV6, &g_CalloutIdAleFlowV6);
    if (!NT_SUCCESS(status)) return status;

    // Stream callout V6
    FWPS_CALLOUT0 calloutStreamV6 = { 0 };
    calloutStreamV6.calloutKey = WFP_MONITOR_CALLOUT_STREAM_V6;
    calloutStreamV6.classifyFn = (FWPS_CALLOUT_CLASSIFY_FN0)StreamClassify;
    calloutStreamV6.notifyFn = (FWPS_CALLOUT_NOTIFY_FN0)StreamNotify;
    calloutStreamV6.flowDeleteFn = (FWPS_CALLOUT_FLOW_DELETE_NOTIFY_FN0)StreamFlowDelete;
    status = FwpsCalloutRegister0(deviceObject, &calloutStreamV6, &g_CalloutIdStreamV6);
    if (!NT_SUCCESS(status)) return status;

    // Add Callouts to WFP Base Filtering Engine
    FWPM_CALLOUT0 mCalloutAle = { 0 };
    mCalloutAle.calloutKey = WFP_MONITOR_CALLOUT_ALE_FLOW_V4;
    mCalloutAle.displayData.name = L"Wfp Monitor ALE Flow Callout";
    mCalloutAle.applicableLayer = FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4;
    status = FwpmCalloutAdd0(g_EngineHandle, &mCalloutAle, NULL, NULL);
    if (!NT_SUCCESS(status)) return status;

    FWPM_CALLOUT0 mCalloutStream = { 0 };
    mCalloutStream.calloutKey = WFP_MONITOR_CALLOUT_STREAM_V4;
    mCalloutStream.displayData.name = L"Wfp Monitor Stream Callout";
    mCalloutStream.applicableLayer = FWPM_LAYER_STREAM_V4;
    status = FwpmCalloutAdd0(g_EngineHandle, &mCalloutStream, NULL, NULL);
    if (!NT_SUCCESS(status)) return status;

    FWPM_CALLOUT0 mCalloutAleV6 = { 0 };
    mCalloutAleV6.calloutKey = WFP_MONITOR_CALLOUT_ALE_FLOW_V6;
    mCalloutAleV6.displayData.name = L"Wfp Monitor ALE Flow Callout V6";
    mCalloutAleV6.applicableLayer = FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6;
    status = FwpmCalloutAdd0(g_EngineHandle, &mCalloutAleV6, NULL, NULL);
    if (!NT_SUCCESS(status)) return status;

    FWPM_CALLOUT0 mCalloutStreamV6 = { 0 };
    mCalloutStreamV6.calloutKey = WFP_MONITOR_CALLOUT_STREAM_V6;
    mCalloutStreamV6.displayData.name = L"Wfp Monitor Stream Callout V6";
    mCalloutStreamV6.applicableLayer = FWPM_LAYER_STREAM_V6;
    status = FwpmCalloutAdd0(g_EngineHandle, &mCalloutStreamV6, NULL, NULL);
    if (!NT_SUCCESS(status)) return status;

    // Add Filters
    FWPM_FILTER0 filterAle = { 0 };
    filterAle.filterKey = WFP_MONITOR_CALLOUT_ALE_FLOW_V4;
    filterAle.displayData.name = L"Wfp Monitor ALE Flow Filter";
    filterAle.layerKey = FWPM_LAYER_ALE_FLOW_ESTABLISHED_V4;
    filterAle.action.type = FWP_ACTION_CALLOUT_TERMINATING;
    filterAle.action.calloutKey = WFP_MONITOR_CALLOUT_ALE_FLOW_V4;
    filterAle.weight.type = FWP_EMPTY;
    status = FwpmFilterAdd0(g_EngineHandle, &filterAle, NULL, &g_FilterIdAleFlowV4);
    if (!NT_SUCCESS(status)) return status;

    FWPM_FILTER0 filterStream = { 0 };
    filterStream.filterKey = WFP_MONITOR_CALLOUT_STREAM_V4;
    filterStream.displayData.name = L"Wfp Monitor Stream Filter";
    filterStream.layerKey = FWPM_LAYER_STREAM_V4;
    filterStream.action.type = FWP_ACTION_CALLOUT_TERMINATING;
    filterStream.action.calloutKey = WFP_MONITOR_CALLOUT_STREAM_V4;
    filterStream.weight.type = FWP_EMPTY;
    status = FwpmFilterAdd0(g_EngineHandle, &filterStream, NULL, &g_FilterIdStreamV4);
    if (!NT_SUCCESS(status)) return status;

    FWPM_FILTER0 filterAleV6 = { 0 };
    filterAleV6.filterKey = WFP_MONITOR_CALLOUT_ALE_FLOW_V6;
    filterAleV6.displayData.name = L"Wfp Monitor ALE Flow Filter V6";
    filterAleV6.layerKey = FWPM_LAYER_ALE_FLOW_ESTABLISHED_V6;
    filterAleV6.action.type = FWP_ACTION_CALLOUT_TERMINATING;
    filterAleV6.action.calloutKey = WFP_MONITOR_CALLOUT_ALE_FLOW_V6;
    filterAleV6.weight.type = FWP_EMPTY;
    status = FwpmFilterAdd0(g_EngineHandle, &filterAleV6, NULL, &g_FilterIdAleFlowV6);
    if (!NT_SUCCESS(status)) return status;

    FWPM_FILTER0 filterStreamV6 = { 0 };
    filterStreamV6.filterKey = WFP_MONITOR_CALLOUT_STREAM_V6;
    filterStreamV6.displayData.name = L"Wfp Monitor Stream Filter V6";
    filterStreamV6.layerKey = FWPM_LAYER_STREAM_V6;
    filterStreamV6.action.type = FWP_ACTION_CALLOUT_TERMINATING;
    filterStreamV6.action.calloutKey = WFP_MONITOR_CALLOUT_STREAM_V6;
    filterStreamV6.weight.type = FWP_EMPTY;
    status = FwpmFilterAdd0(g_EngineHandle, &filterStreamV6, NULL, &g_FilterIdStreamV6);

    return status;
}

void UnregisterCallouts()
{
    if (g_EngineHandle != NULL) {
        if (g_FilterIdAleFlowV4 != 0) {
            FwpmFilterDeleteById0(g_EngineHandle, g_FilterIdAleFlowV4);
            g_FilterIdAleFlowV4 = 0;
        }
        if (g_FilterIdStreamV4 != 0) {
            FwpmFilterDeleteById0(g_EngineHandle, g_FilterIdStreamV4);
            g_FilterIdStreamV4 = 0;
        }
        if (g_FilterIdAleFlowV6 != 0) {
            FwpmFilterDeleteById0(g_EngineHandle, g_FilterIdAleFlowV6);
            g_FilterIdAleFlowV6 = 0;
        }
        if (g_FilterIdStreamV6 != 0) {
            FwpmFilterDeleteById0(g_EngineHandle, g_FilterIdStreamV6);
            g_FilterIdStreamV6 = 0;
        }
        FwpmCalloutDeleteById0(g_EngineHandle, g_CalloutIdAleFlowV4);
        FwpmCalloutDeleteById0(g_EngineHandle, g_CalloutIdStreamV4);
        FwpmCalloutDeleteById0(g_EngineHandle, g_CalloutIdAleFlowV6);
        FwpmCalloutDeleteById0(g_EngineHandle, g_CalloutIdStreamV6);
    }

    if (g_CalloutIdAleFlowV4 != 0) {
        FwpsCalloutUnregisterById0(g_CalloutIdAleFlowV4);
        g_CalloutIdAleFlowV4 = 0;
    }
    if (g_CalloutIdStreamV4 != 0) {
        FwpsCalloutUnregisterById0(g_CalloutIdStreamV4);
        g_CalloutIdStreamV4 = 0;
    }
    if (g_CalloutIdAleFlowV6 != 0) {
        FwpsCalloutUnregisterById0(g_CalloutIdAleFlowV6);
        g_CalloutIdAleFlowV6 = 0;
    }
    if (g_CalloutIdStreamV6 != 0) {
        FwpsCalloutUnregisterById0(g_CalloutIdStreamV6);
        g_CalloutIdStreamV6 = 0;
    }
}

// ALE Flow Established
void AleFlowEstablishedClassify(
    const FWPS_INCOMING_VALUES0* inFixedValues,
    const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    void* layerData,
    const void* classifyContext,
    const FWPS_FILTER0* filter,
    UINT64 flowContext,
    FWPS_CLASSIFY_OUT0* classifyOut
)
{
    UNREFERENCED_PARAMETER(layerData);
    UNREFERENCED_PARAMETER(classifyContext);
    UNREFERENCED_PARAMETER(filter);
    UNREFERENCED_PARAMETER(flowContext);

    classifyOut->actionType = FWP_ACTION_PERMIT;

    KIRQL oldIrql;
    KeAcquireSpinLock(&g_StatsLock, &oldIrql);
    g_Stats.DebugCallAle++;
    if (inMetaValues->currentMetadataValues & FWPS_METADATA_FIELD_PROCESS_ID) {
        g_Stats.DebugLastSeenPid = (ULONG)inMetaValues->processId;
    }
    KeReleaseSpinLock(&g_StatsLock, oldIrql);

    // CRITICAL: We do association even if rights check fails (read-only mode)
    // but we check for metadata availability first.
    if (!(inMetaValues->currentMetadataValues & FWPS_METADATA_FIELD_PROCESS_ID) ||
        !(inMetaValues->currentMetadataValues & FWPS_METADATA_FIELD_FLOW_HANDLE)) {
        return;
    }

    ULONG processId = (ULONG)inMetaValues->processId;
    ULONG targetPid;
    KeAcquireSpinLock(&g_StatsLock, &oldIrql);
    targetPid = g_TargetPid;
    KeReleaseSpinLock(&g_StatsLock, oldIrql);

    if (targetPid > 0 && processId == targetPid) {
        KeAcquireSpinLock(&g_StatsLock, &oldIrql);
        g_Stats.DebugMatchPid++;
        g_Stats.DebugAssocAttempt++;
        KeReleaseSpinLock(&g_StatsLock, oldIrql);
        
        NTSTATUS status = STATUS_SUCCESS;
        if (inFixedValues->layerId == FWPS_LAYER_ALE_FLOW_ESTABLISHED_V4) {
            status = FwpsFlowAssociateContext0(inMetaValues->flowHandle, FWPS_LAYER_STREAM_V4, g_CalloutIdStreamV4, g_FlowContextValue);
            if (NT_SUCCESS(status)) {
                if (inFixedValues->incomingValue[FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_REMOTE_ADDRESS].value.type == FWP_UINT32) {
                    UINT32 destIp = inFixedValues->incomingValue[FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_REMOTE_ADDRESS].value.uint32;
                    UINT16 destPort = inFixedValues->incomingValue[FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_REMOTE_PORT].value.uint16;
                    KeAcquireSpinLock(&g_StatsLock, &oldIrql);
                    g_Stats.DestIp = destIp; // Keep network byte order for inet_ntoa
                    g_Stats.DestPort = destPort; // Keep network byte order for ntohs
                    KeReleaseSpinLock(&g_StatsLock, oldIrql);
                }
            }
        } else if (inFixedValues->layerId == FWPS_LAYER_ALE_FLOW_ESTABLISHED_V6) {
            status = FwpsFlowAssociateContext0(inMetaValues->flowHandle, FWPS_LAYER_STREAM_V6, g_CalloutIdStreamV6, g_FlowContextValue);
        }

        KeAcquireSpinLock(&g_StatsLock, &oldIrql);
        g_Stats.DebugAssocStatus = (ULONG)status;
        KeReleaseSpinLock(&g_StatsLock, oldIrql);
    }
}

NTSTATUS AleFlowEstablishedNotify(FWPS_CALLOUT_NOTIFY_TYPE notifyType, const GUID* filterKey, FWPS_FILTER0* filter)
{
    UNREFERENCED_PARAMETER(notifyType);
    UNREFERENCED_PARAMETER(filterKey);
    UNREFERENCED_PARAMETER(filter);
    return STATUS_SUCCESS;
}

// Stream callout
void StreamClassify(
    const FWPS_INCOMING_VALUES0* inFixedValues,
    const FWPS_INCOMING_METADATA_VALUES0* inMetaValues,
    void* layerData,
    const void* classifyContext,
    const FWPS_FILTER0* filter,
    UINT64 flowContext,
    FWPS_CLASSIFY_OUT0* classifyOut
)
{
    UNREFERENCED_PARAMETER(inMetaValues);
    UNREFERENCED_PARAMETER(classifyContext);
    UNREFERENCED_PARAMETER(filter);

    classifyOut->actionType = FWP_ACTION_PERMIT;
    
    KIRQL oldIrql;
    KeAcquireSpinLock(&g_StatsLock, &oldIrql);
    g_Stats.DebugStreamCall++;
    KeReleaseSpinLock(&g_StatsLock, oldIrql);

    if ((classifyOut->rights & FWPS_RIGHT_ACTION_WRITE) == 0) return;

    if (flowContext == g_FlowContextValue) {
        KeAcquireSpinLock(&g_StatsLock, &oldIrql);
        g_Stats.DebugMatchContext++;
        KeReleaseSpinLock(&g_StatsLock, oldIrql);
        
        FWPS_STREAM_CALLOUT_IO_PACKET0* streamPacket = (FWPS_STREAM_CALLOUT_IO_PACKET0*)layerData;
        if (streamPacket && streamPacket->streamData != NULL && streamPacket->streamData->dataLength > 0) {
            UINT32 directionField = (inFixedValues->layerId == FWPS_LAYER_STREAM_V4) ? FWPS_FIELD_STREAM_V4_DIRECTION : FWPS_FIELD_STREAM_V6_DIRECTION;
            UINT32 direction = inFixedValues->incomingValue[directionField].value.uint32;
            KeAcquireSpinLock(&g_StatsLock, &oldIrql);
            if (direction == FWP_DIRECTION_OUTBOUND) {
                g_Stats.TxBytes += streamPacket->streamData->dataLength;
            } else if (direction == FWP_DIRECTION_INBOUND) {
                g_Stats.RxBytes += streamPacket->streamData->dataLength;
            }
            KeReleaseSpinLock(&g_StatsLock, oldIrql);
        }
    }
}

NTSTATUS StreamNotify(FWPS_CALLOUT_NOTIFY_TYPE notifyType, const GUID* filterKey, FWPS_FILTER0* filter)
{
    UNREFERENCED_PARAMETER(notifyType);
    UNREFERENCED_PARAMETER(filterKey);
    UNREFERENCED_PARAMETER(filter);
    return STATUS_SUCCESS;
}

void StreamFlowDelete(UINT16 layerId, UINT32 calloutId, UINT64 flowContext)
{
    UNREFERENCED_PARAMETER(layerId);
    UNREFERENCED_PARAMETER(calloutId);
    UNREFERENCED_PARAMETER(flowContext);
}
