#include <windows.h>

#include <stdio.h>

#define _NTSCSI_USER_MODE_
#include <scsi.h>

#include "wvbd.h"

VOID LogMessage(PWVBD_DEVICE Device, WvbdLogLevel LogLevel,
                const char* FileName, UINT32 Line, const char* FunctionName,
                const char* Format, ...) {
    if (!Device || !Device->Interface->LogMessage ||
            Device->LogLevel < LogLevel)
        return;

    va_list Args;
    va_start(Args, Format);

    size_t BufferLength = _vscprintf(Format, Args) + 1;
    char* Buff = (char*) malloc(BufferLength);
    if (!Buff)
        return;

    vsnprintf_s(Buff, BufferLength, BufferLength - 1, Format, Args);
    va_end(Args);

    Device->Interface->LogMessage(
        Device, LogLevel, Buff,
        FileName, Line, FunctionName);

    free(Buff);
}

#define LogCritical(Device, Format, ...) \
    LogMessage(Device, WvbdLogLevelCritical, \
               __FILE__, __LINE__, __FUNCTION__, Format, __VA_ARGS__)
#define LogError(Device, Format, ...) \
    LogMessage(Device, WvbdLogLevelError, \
               __FILE__, __LINE__, __FUNCTION__, Format, __VA_ARGS__)
#define LogWarning(Device, Format, ...) \
    LogMessage(Device, WvbdLogLevelWarning, \
               __FILE__, __LINE__, __FUNCTION__, Format, __VA_ARGS__)
#define LogInfo(Device, Format, ...) \
    LogMessage(Device, WvbdLogLevelInfo, \
               __FILE__, __LINE__, __FUNCTION__, Format, __VA_ARGS__)
#define LogDebug(Device, Format, ...) \
    LogMessage(Device, WvbdLogLevelDebug, \
               __FILE__, __LINE__, __FUNCTION__, Format, __VA_ARGS__)
#define LogTrace(Device, Format, ...) \
    LogMessage(Device, WvbdLogLevelTrace, \
               __FILE__, __LINE__, __FUNCTION__, Format, __VA_ARGS__)

DWORD WvbdCreate(
    const PWVBD_PROPERTIES Properties,
    const PWVBD_INTERFACE Interface,
    PVOID Context,
    WvbdLogLevel LogLevel,
    PWVBD_DEVICE* PDevice)
{
    DWORD ErrorCode = ERROR_SUCCESS;
    PWVBD_DEVICE Device = NULL;

    if (STRING_OVERFLOWS(Properties->InstanceName, MAX_NAME_LENGTH)) {
        return ERROR_BUFFER_OVERFLOW;
    }

    Device = (PWVBD_DEVICE)calloc(1, sizeof(WVBD_DEVICE));
    if (!Device) {
        return ERROR_OUTOFMEMORY;
    }

    Device->Context = Context;
    Device->Interface = Interface;
    Device->Properties = *Properties;
    Device->LogLevel = LogLevel;
    Device->Handle = WvbdOpenDevice();

    LogDebug(Device,
             "Mapping device. Name=%s, BC=%llu, BS=%lu, RO=%u, "
             "Cache=%u, Unmap=%u, UnmapAnchor=%u, MaxUnmapDescCount=%u, "
             "MaxTransferLength=%u.",
             Properties->InstanceName,
             Properties->BlockCount,
             Properties->BlockSize,
             Properties->ReadOnly,
             Properties->CacheSupported,
             Properties->UnmapSupported,
             Properties->UnmapAnchorSupported,
             Properties->MaxUnmapDescCount,
             Properties->MaxTransferLength);

    if (Device->Handle == INVALID_HANDLE_VALUE || !Device->Handle) {
        ErrorCode = ERROR_OPEN_FAILED;
        LogError(Device,
                 "Could not oped WVBD device. Please make sure "
                 "that the driver is installed.");
        goto Exit;
    }

    ErrorCode = WvbdIoctlCreate(
        Device->Handle, &Device->Properties, &Device->DiskHandle);
    if (ErrorCode) {
        LogError(Device, "Could not map WVBD virtual disk.");
        goto Exit;
    }

    LogDebug(Device, "Mapped device. Handle: %llu.", Device->DiskHandle);

    *PDevice = Device;

Exit:
    if (ErrorCode) {
        WvbdClose(Device);
    }

    return ErrorCode;
}

DWORD WvbdRemove(PWVBD_DEVICE Device)
{
    // TODO: check for null pointers.
    DWORD ErrorCode = ERROR_SUCCESS;

    LogDebug(Device, "Unmapping device %s.",
             Device->Properties.InstanceName);
    if (Device->Handle == INVALID_HANDLE_VALUE) {
        LogDebug(Device, "WVBD device already removed.");
        return 0;
    }

    ErrorCode = WvbdIoctlRemove(Device->Handle, Device->Properties.InstanceName);
    if (ErrorCode && ErrorCode != ERROR_FILE_NOT_FOUND) {
        LogError(Device, "Could not remove WVBD virtual disk. Error: %d.", ErrorCode);
    }
    else {
        Device->Handle = INVALID_HANDLE_VALUE;
    }

    return ErrorCode;
}

void WvbdClose(PWVBD_DEVICE Device)
{
    if (!Device)
        return;

    LogDebug(Device, "Closing device");
    if (Device->Handle)
        CloseHandle(Device->Handle);

    if (Device->DispatcherThreads)
        free(Device->DispatcherThreads);

    free(Device);
}

VOID WvbdSignalStopped(PWVBD_DEVICE Device)
{
    LogDebug(Device, "Marking device as stopped.");
    if (Device)
        Device->Stopped = TRUE;
}

BOOLEAN WvbdIsStopped(PWVBD_DEVICE Device)
{
    return Device->Stopped;
}

BOOLEAN WvbdIsRunning(PWVBD_DEVICE Device)
{
    return Device->Started && !WvbdIsStopped(Device);
}

DWORD WvbdStopDispatcher(PWVBD_DEVICE Device)
{
    // By not setting the "Stopped" event, we allow the driver to finish
    // pending IO requests, which we'll continue processing until getting
    // the "Disconnect" request from the driver.
    DWORD Ret = 0;

    LogDebug(Device, "Stopping dispatcher.");
    if (!InterlockedExchange8(&Device->Stopping, 1)) {
        Ret = WvbdRemove(Device);
    }

    return Ret;
}

void WvbdSetSenseEx(PWVBD_STATUS Status, UINT8 SenseKey, UINT8 Asc, UINT64 Info)
{
    Status->ScsiStatus = SCSISTAT_CHECK_CONDITION;
    Status->SenseKey = SenseKey;
    Status->ASC = Asc;
    Status->Information = Info;
    Status->InformationValid = 1;
}

void WvbdSetSense(PWVBD_STATUS Status, UINT8 SenseKey, UINT8 Asc)
{
    Status->ScsiStatus = SCSISTAT_CHECK_CONDITION;
    Status->SenseKey = SenseKey;
    Status->ASC = Asc;
    Status->Information = 0;
    Status->InformationValid = 0;
}

DWORD WvbdSendResponse(
    PWVBD_DEVICE Device,
    PWVBD_IO_RESPONSE Response,
    PVOID DataBuffer,
    UINT32 DataBufferSize)
{
    LogDebug(
        Device,
        "Sending response: [%s] : (SS:%u, SK:%u, ASC:%u, I:%llu) # %llx "
        "@ %p~0x%x.",
        WvbdRequestTypeToStr(Response->RequestType),
        Response->Status.ScsiStatus,
        Response->Status.SenseKey,
        Response->Status.ASC,
        Response->Status.InformationValid ?
            Response->Status.Information : 0,
        Response->RequestHandle,
        DataBuffer,
        DataBufferSize);

    InterlockedIncrement64(&Device->Stats.TotalReceivedReplies);
    InterlockedDecrement64(&Device->Stats.PendingSubmittedRequests);

    if (Response->Status.ScsiStatus) {
        switch(Response->RequestType) {
            case WvbdReqTypeRead:
                InterlockedIncrement64(&Device->Stats.ReadErrors);
            case WvbdReqTypeWrite:
                InterlockedIncrement64(&Device->Stats.WriteErrors);
            case WvbdReqTypeFlush:
                InterlockedIncrement64(&Device->Stats.FlushErrors);
            case WvbdReqTypeUnmap:
                InterlockedIncrement64(&Device->Stats.UnmapErrors);
        }
    }

    if (!WvbdIsRunning(Device)) {
        LogDebug(Device, "Device disconnected, cannot send response.");
        return ERROR_PIPE_NOT_CONNECTED;
    }

    return WvbdIoctlSendResponse(
        Device->Handle,
        Device->DiskHandle,
        Response,
        DataBuffer,
        DataBufferSize);
}

VOID WvbdHandleRequest(PWVBD_DEVICE Device, PWVBD_IO_REQUEST Request,
                       PVOID Buffer)
{
    UINT8 AdditionalSenseCode = 0;
    BOOLEAN IsValid = TRUE;

    InterlockedIncrement64(&Device->Stats.TotalReceivedRequests);
    InterlockedIncrement64(&Device->Stats.UnsubmittedRequests);

    switch (Request->RequestType) {
        case WvbdReqTypeDisconnect:
            LogInfo(Device, "Received disconnect request.");
            WvbdSignalStopped(Device);
            break;
        case WvbdReqTypeRead:
            if (!Device->Interface->Read)
                goto Unsupported;
            LogDebug(Device, "Dispatching READ @ 0x%llx~0x%x # %llx.",
                     Request->Cmd.Read.BlockAddress,
                     Request->Cmd.Read.BlockCount,
                     Request->RequestHandle);
            Device->Interface->Read(
                Device,
                Request->RequestHandle,
                Buffer,
                Request->Cmd.Read.BlockAddress,
                Request->Cmd.Read.BlockCount,
                Request->Cmd.Read.ForceUnitAccess);

            InterlockedIncrement64(&Device->Stats.TotalRWRequests);
            InterlockedAdd64(&Device->Stats.TotalReadBlocks,
                             Request->Cmd.Read.BlockCount);
            break;
        case WvbdReqTypeWrite:
            if (!Device->Interface->Write)
                goto Unsupported;
            LogDebug(Device, "Dispatching WRITE @ 0x%llx~0x%x # %llx." ,
                     Request->Cmd.Write.BlockAddress,
                     Request->Cmd.Write.BlockCount,
                     Request->RequestHandle);
            Device->Interface->Write(
                Device,
                Request->RequestHandle,
                Buffer,
                Request->Cmd.Write.BlockAddress,
                Request->Cmd.Write.BlockCount,
                Request->Cmd.Write.ForceUnitAccess);

            InterlockedIncrement64(&Device->Stats.TotalRWRequests);
            InterlockedAdd64(&Device->Stats.TotalWrittenBlocks,
                             Request->Cmd.Write.BlockCount);
            break;
        case WvbdReqTypeFlush:
            // TODO: should it be a no-op when unsupported?
            if (!Device->Interface->Flush || !Device->Properties.CacheSupported)
                goto Unsupported;
            LogDebug(Device, "Dispatching FLUSH @ 0x%llx~0x%x # %llx." ,
                     Request->Cmd.Flush.BlockAddress,
                     Request->Cmd.Flush.BlockCount,
                     Request->RequestHandle);
            Device->Interface->Flush(
                Device->Handle,
                Request->RequestHandle,
                Request->Cmd.Flush.BlockAddress,
                Request->Cmd.Flush.BlockCount);
        case WvbdReqTypeUnmap:
            if (!Device->Interface->Unmap || !Device->Properties.UnmapSupported)
                goto Unsupported;
            if (!Device->Properties.UnmapAnchorSupported &&
                Request->Cmd.Unmap.Anchor)
            {
                AdditionalSenseCode = SCSI_ADSENSE_INVALID_CDB;
                goto Unsupported;
            }

            if (Device->Properties.MaxUnmapDescCount <=
                Request->Cmd.Unmap.Count)
            {
                AdditionalSenseCode = SCSI_ADSENSE_INVALID_FIELD_PARAMETER_LIST;
                goto Unsupported;
            }

            LogDebug(Device, "Dispatching UNMAP # %llx.",
                     Request->RequestHandle);
            Device->Interface->Unmap(
                Device->Handle,
                Request->RequestHandle,
                Buffer,
                Request->Cmd.Unmap.Count);
        default:
        Unsupported:
            LogDebug(Device, "Received unsupported command. "
                     "Request type: %d, request handle: %llu.",
                     Request->RequestType,
                     Request->RequestHandle);
            IsValid = FALSE;
            if (!AdditionalSenseCode)
                AdditionalSenseCode = SCSI_ADSENSE_ILLEGAL_COMMAND;

            WVBD_IO_RESPONSE Response = { 0 };
            Response.RequestHandle = Request->RequestHandle;
            WvbdSetSense(&Response.Status,
                         SCSI_SENSE_ILLEGAL_REQUEST,
                         AdditionalSenseCode);
            WvbdSendResponse(Device, &Response, NULL, 0);
            break;
    }

    InterlockedDecrement64(&Device->Stats.UnsubmittedRequests);
    if (IsValid) {
        InterlockedIncrement64(&Device->Stats.TotalSubmittedRequests);
        InterlockedIncrement64(&Device->Stats.PendingSubmittedRequests);
    } else {
        InterlockedIncrement64(&Device->Stats.InvalidRequests);
    }
}

DWORD WvbdDispatcherLoop(PWVBD_DEVICE Device)
{
    DWORD ErrorCode = 0;
    WVBD_IO_REQUEST Request;
    PVOID Buffer = malloc(Device->Properties.MaxTransferLength);

    // gruesome hack to give storport enough time to identify our device.
    // TODO: find a better way of handling pending disks.
    Sleep(5000);

    while (WvbdIsRunning(Device)) {
        ErrorCode = WvbdIoctlFetchRequest(
            Device->Handle,
            Device->DiskHandle,
            &Request,
            Buffer,
            Device->Properties.MaxTransferLength);
        if (ErrorCode) {
            LogWarning(Device, "Could not fetch request: %d", ErrorCode);
            break;
        }
        WvbdHandleRequest(Device, &Request, Buffer);
    }

    WvbdStopDispatcher(Device);
    free(Buffer);

    return ErrorCode;
}

DWORD WvbdStartDispatcher(PWVBD_DEVICE Device, DWORD ThreadCount)
{
    DWORD ErrorCode = ERROR_SUCCESS;
    if (ThreadCount < WVBD_MIN_DISPATCHER_THREAD_COUNT ||
       ThreadCount > WVBD_MAX_DISPATCHER_THREAD_COUNT) {
        LogError(Device, "Invalid number of dispatcher threads: %u",
                 ThreadCount);
        return ERROR_INVALID_PARAMETER;
    }

    LogDebug(Device, "Starting dispatcher. Threads: %u", ThreadCount);
    Device->DispatcherThreads = (HANDLE*)malloc(sizeof(HANDLE) * ThreadCount);
    if (!Device->DispatcherThreads) {
        LogError(Device, "Could not allocate memory.");
        return ERROR_OUTOFMEMORY;
    }

    Device->Started = TRUE;
    Device->DispatcherThreadsCount = 0;

    for (DWORD i = 0; i < ThreadCount; i++)
    {
        HANDLE Thread = CreateThread(0, 0, WvbdDispatcherLoop, Device, 0, 0);
        if (!Thread)
        {
            LogError(Device, "Could not start dispatcher thread.");
            ErrorCode = GetLastError();
            WvbdStopDispatcher(Device);
            WvbdWaitDispatcher(Device);
            break;
        }
        Device->DispatcherThreads[Device->DispatcherThreadsCount] = Thread;
        Device->DispatcherThreadsCount++;
    }

    return ErrorCode;
}

DWORD WvbdWaitDispatcher(PWVBD_DEVICE Device)
{
    LogDebug(Device, "Waiting for the dispatcher to stop.");
    if (!Device->Started) {
        LogError(Device, "The dispatcher hasn't been started.");
        return ERROR_PIPE_NOT_CONNECTED;
    }

    if (!Device->DispatcherThreads || !Device->DispatcherThreadsCount ||
            !WvbdIsRunning(Device)) {
        LogInfo(Device, "The dispatcher isn't running.");
        return 0;
    }

    DWORD Ret = WaitForMultipleObjects(
        Device->DispatcherThreadsCount,
        Device->DispatcherThreads,
        TRUE, // WaitAll
        INFINITE);

    if (Ret == WAIT_FAILED) {
        DWORD Err = GetLastError();
        LogError(Device, "Failed waiting for the dispatcher. Error: %d", Err);
        return Err;
    }

    LogDebug(Device, "The dispatcher stopped.");
    return 0;
}
