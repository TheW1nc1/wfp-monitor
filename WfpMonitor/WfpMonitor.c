#include <ntddk.h>
#pragma warning(push)
#pragma warning(disable:4201)       // unnamed struct/union
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
UINT64 g_FilterIdAleFlowV4 = 0;
UINT64 g_FilterIdStreamV4 = 0;

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
    RtlInitUnicodeString(&symLinkName, WFP_MONITOR_SYMLINK_NAME);

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

    RtlInitUnicodeString(&symLinkName, WFP_MONITOR_SYMLINK_NAME);
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
// {A55F5A11-E623-45C1-A528-56BCA6FD8B2A}
DEFINE_GUID(WFP_MONITOR_CALLOUT_ALE_FLOW_V4, 0xa55f5a11, 0xe623, 0x45c1, 0xa5, 0x28, 0x56, 0xbc, 0xa6, 0xfd, 0x8b, 0x2a);
// {B66F6B22-E623-45C1-A528-56BCA6FD8B2B}
DEFINE_GUID(WFP_MONITOR_CALLOUT_STREAM_V4, 0xb66f6b22, 0xe623, 0x45c1, 0xa5, 0x28, 0x56, 0xbc, 0xa6, 0xfd, 0x8b, 0x2b);

NTSTATUS RegisterCallouts(DEVICE_OBJECT* deviceObject)
{
    NTSTATUS status;

    // 1. ALE Flow Established Callout structure
    FWPS_CALLOUT0 calloutAle = { 0 };
    calloutAle.calloutKey = WFP_MONITOR_CALLOUT_ALE_FLOW_V4;
    calloutAle.classifyFn = AleFlowEstablishedClassify;
    calloutAle.notifyFn = AleFlowEstablishedNotify;
    calloutAle.flowDeleteFn = NULL;

    status = FwpsCalloutRegister0(deviceObject, &calloutAle, &g_CalloutIdAleFlowV4);
    if (!NT_SUCCESS(status)) return status;

    // Stream Callout structure
    FWPS_CALLOUT0 calloutStream = { 0 };
    calloutStream.calloutKey = WFP_MONITOR_CALLOUT_STREAM_V4;
    calloutStream.classifyFn = StreamClassify;
    calloutStream.notifyFn = StreamNotify;
    calloutStream.flowDeleteFn = StreamFlowDelete;

    status = FwpsCalloutRegister0(deviceObject, &calloutStream, &g_CalloutIdStreamV4);
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
        FwpmCalloutDeleteById0(g_EngineHandle, g_CalloutIdAleFlowV4);
        FwpmCalloutDeleteById0(g_EngineHandle, g_CalloutIdStreamV4);
    }

    if (g_CalloutIdAleFlowV4 != 0) {
        FwpsCalloutUnregisterById0(g_CalloutIdAleFlowV4);
        g_CalloutIdAleFlowV4 = 0;
    }
    if (g_CalloutIdStreamV4 != 0) {
        FwpsCalloutUnregisterById0(g_CalloutIdStreamV4);
        g_CalloutIdStreamV4 = 0;
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

    if ((classifyOut->rights & FWPS_RIGHT_ACTION_WRITE) == 0) return;

    if (!(inMetaValues->currentMetadataValues & FWPS_METADATA_FIELD_PROCESS_ID) ||
        !(inMetaValues->currentMetadataValues & FWPS_METADATA_FIELD_FLOW_HANDLE)) {
        return;
    }

    ULONG processId = (ULONG)inMetaValues->processId;
    
    KIRQL oldIrql;
    ULONG targetPid;
    
    KeAcquireSpinLock(&g_StatsLock, &oldIrql);
    targetPid = g_TargetPid;
    KeReleaseSpinLock(&g_StatsLock, oldIrql);

    // If target PID matches, we monitor this flow
    if (targetPid > 0 && processId == targetPid) {
        
        // Setup flow context
        NTSTATUS status = FwpsFlowAssociateContext0(inMetaValues->flowHandle, FWPS_LAYER_STREAM_V4, g_CalloutIdStreamV4, g_FlowContextValue);
        
        if (NT_SUCCESS(status)) {
            // Assume IPv4, record Destination IP and Port
            if (inFixedValues->incomingValue[FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_REMOTE_ADDRESS].value.type == FWP_UINT32) {
                UINT32 destIp = inFixedValues->incomingValue[FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_REMOTE_ADDRESS].value.uint32;
                UINT16 destPort = inFixedValues->incomingValue[FWPS_FIELD_ALE_FLOW_ESTABLISHED_V4_IP_REMOTE_PORT].value.uint16;

                KeAcquireSpinLock(&g_StatsLock, &oldIrql);
                // Record the first destination encountered.
                // Or if we want to overwrite, we can just do:
                g_Stats.DestIp = destIp;     // host byte order -> need to use RtlUlongByteSwap later or just keep as is
                g_Stats.DestPort = destPort; // host byte order
                KeReleaseSpinLock(&g_StatsLock, oldIrql);
            }
        }
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
    if ((classifyOut->rights & FWPS_RIGHT_ACTION_WRITE) == 0) return;

    if (flowContext == g_FlowContextValue) {
        FWPS_STREAM_CALLOUT_IO_PACKET0* streamPacket = (FWPS_STREAM_CALLOUT_IO_PACKET0*)layerData;
        if (streamPacket && streamPacket->streamData != NULL && streamPacket->streamData->dataLength > 0) {
            
            UINT32 direction = inFixedValues->incomingValue[FWPS_FIELD_STREAM_V4_DIRECTION].value.uint32;
            
            KIRQL oldIrql;
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
