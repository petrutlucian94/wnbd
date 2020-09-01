/*
 * Copyright (c) 2019 SUSE LLC
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#include "cmd.h"
#include "wvbd.h"
#include "rbd_protocol.h"

#include <string>
#include <codecvt>
#include <locale>

#pragma comment(lib, "Setupapi.lib")

using convert_t = std::codecvt_utf8<wchar_t>;
std::wstring_convert<convert_t, wchar_t> strconverter;

std::string to_string(std::wstring wstr)
{
    return strconverter.to_bytes(wstr);
}

std::wstring to_wstring(std::string str)
{
    return strconverter.from_bytes(str);
}

void PrintSyntax()
{
    fprintf(stderr,"Syntax:\n");
    fprintf(stderr, "wnbd-client map  <InstanceName> <HostName> "
                    "<PortName> <ExportName> <DoNotNegotiate> <ReadOnly>\n");
    fprintf(stderr,"wnbd-client unmap <InstanceName>\n");
    fprintf(stderr,"wnbd-client list \n");
    fprintf(stderr,"wnbd-client set-debug <int>\n");
    fprintf(stderr,"wnbd-client stats <InstanceName>\n");
}

void PrintFormattedError(DWORD Error)
{
    LPVOID LpMsgBuf;

    FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        Error,
        0,
        (LPTSTR)&LpMsgBuf,
        0,
        NULL
    );

    fprintf(stderr, "Error code: %d. Error message: %s\n", Error, (LPTSTR)LpMsgBuf);

    LocalFree(LpMsgBuf);
}

void PrintLastError()
{
    PrintFormattedError(GetLastError());
}

// We're doing this check quite often, so this little helper comes in handy.
BOOLEAN CheckOpenFailed(DWORD Status)
{
    if (Status == ERROR_OPEN_FAILED) {
        fprintf(stderr,
                "Could not open WNBD device. Make sure that the driver "
                "is installed.\n");
        PrintFormattedError(Status);
    }
    return Status == ERROR_OPEN_FAILED;
}

DWORD CmdMap(
    PCHAR InstanceName,
    PCHAR HostName,
    PCHAR PortName,
    PCHAR ExportName,
    UINT64 DiskSize,
    BOOLEAN MustNegotiate,
    BOOLEAN ReadOnly)
{
    // TODO: switch to the new API
    CONNECTION_INFO ConnectIn = { 0 };
    DWORD BytesReturned = 0;
    BOOL DevStatus = 0;
    INT Pid = _getpid();
    UINT16 NbdFlags = 0;
    if(ReadOnly) {
        NbdFlags = NBD_FLAG_HAS_FLAGS;
        NbdFlags |= NBD_FLAG_READ_ONLY;
    }

    HANDLE WnbdDriverHandle = INVALID_HANDLE_VALUE;
    DWORD Status = WvbdOpenDevice(&WnbdDriverHandle);
    if (CheckOpenFailed(Status))
        return Status;

    memcpy(&ConnectIn.InstanceName, InstanceName, min(strlen(InstanceName)+1, MAX_NAME_LENGTH));
    memcpy(&ConnectIn.Hostname, HostName, min(strlen(HostName)+1, MAX_NAME_LENGTH));
    memcpy(&ConnectIn.PortName, PortName, min(strlen(PortName)+1, MAX_NAME_LENGTH));
    memcpy(&ConnectIn.ExportName, ExportName, min(strlen(ExportName)+1, MAX_NAME_LENGTH));
    memcpy(&ConnectIn.SerialNumber, InstanceName, min(strlen(InstanceName)+1, MAX_NAME_LENGTH));
    ConnectIn.DiskSize = DiskSize;
    ConnectIn.IoControlCode = IOCTL_WNBD_MAP;
    ConnectIn.Pid = Pid;
    ConnectIn.MustNegotiate = MustNegotiate;
    ConnectIn.BlockSize = 0;
    ConnectIn.NbdFlags = NbdFlags;

    DevStatus = DeviceIoControl(WnbdDriverHandle, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
                                &ConnectIn, sizeof(CONNECTION_INFO),
        NULL, 0, &BytesReturned, NULL);

    if (!DevStatus) {
        Status = GetLastError();
        fprintf(stderr, "Could not create mapping.\n");
        PrintLastError();
    }

    CloseHandle(WnbdDriverHandle);
    return Status;
}


DWORD CmdUnmap(PCHAR InstanceName)
{
    DWORD Status = WvbdRemoveEx(InstanceName);
    if (Status && !CheckOpenFailed(Status)) {
        fprintf(stderr, "Could not disconnect WNBD device.\n");
        PrintFormattedError(Status);
    }
    return Status;
}

DWORD CmdStats(PCHAR InstanceName)
{
    DWORD Status = ERROR_SUCCESS;
    DWORD BufferSize = 0;
    PWVBD_DRV_STATS Stats = NULL;

    Status = WvbdGetDriverStats(InstanceName, NULL, &BufferSize);
    if (CheckOpenFailed(Status)) {
        goto Exit;
    }
    if (Status && Status != ERROR_INSUFFICIENT_BUFFER) {
        fprintf(stderr, "Could not get stats.\n");
        PrintFormattedError(Status);
        goto Exit;
    }
    if (sizeof(WVBD_DRV_STATS) != BufferSize) {
        fprintf(stderr,
                "Warning: different buffer size received (%zu != %d), "
                "possible version mismatch.\n", sizeof(WVBD_STATS), BufferSize);
    }

    Stats = (PWVBD_DRV_STATS) malloc(BufferSize);
    Status = WvbdGetDriverStats(InstanceName, Stats, &BufferSize);
    if (Status) {
        fprintf(stderr, "Could not get stats.\n");
        PrintFormattedError(Status);
        goto Exit;
    }

    printf("Device stats:\n");
    printf("TotalReceivedIORequests: %llu\n", Stats->TotalReceivedIORequests);
    printf("TotalSubmittedIORequests: %llu\n", Stats->TotalSubmittedIORequests);
    printf("TotalReceivedIOReplies: %llu\n", Stats->TotalReceivedIOReplies);
    printf("UnsubmittedIORequests: %llu\n", Stats->UnsubmittedIORequests);
    printf("PendingSubmittedIORequests: %llu\n", Stats->PendingSubmittedIORequests);
    printf("AbortedSubmittedIORequests: %llu\n", Stats->AbortedSubmittedIORequests);
    printf("AbortedUnsubmittedIORequests: %llu\n", Stats->AbortedUnsubmittedIORequests);
    printf("CompletedAbortedIORequests: %llu\n", Stats->CompletedAbortedIORequests);

Exit:
    if (Stats)
        free(Stats);
    return Status;
}


DWORD GetList(PWVBD_CONNECTION_LIST* ConnectionList)
{
    DWORD Status = WvbdList(ConnectionList);
    if (Status && !CheckOpenFailed(Status)) {
        fprintf(stderr, "Could not get connection list.\n");
        PrintFormattedError(Status);
    }
    if (!Status && sizeof(WVBD_CONNECTION_INFO) != (*ConnectionList)->ElementSize) {
        fprintf(stderr, "Invalid connection info size (%zu != %d). "
                        "Possible version mismatch.\n",
               sizeof(WVBD_CONNECTION_INFO), (*ConnectionList)->ElementSize);
        free(ConnectionList);
        return ERROR_BAD_LENGTH;
    }
    return Status;
}

DWORD CmdList()
{
    PWVBD_CONNECTION_LIST ConnList = NULL;
    HRESULT hres = GetList(&ConnList);
    if (FAILED(hres)) {
        return HRESULT_CODE(hres);
    }

    // This must be called only once.
    hres = WvbdCoInitializeBasic();
    if (FAILED(hres)) {
        fprintf(stderr, "Failed to initialize COM. HRESULT: 0x%x.\n", hres);
        free(ConnList);
        return HRESULT_CODE(hres);
    }

    DWORD Status = 0;
    printf("%-10s  %-10s %s\n", "Pid", "DiskNumber", "InstanceName");
    for (ULONG index = 0; index < ConnList->Count; index++) {
        std::wstring SerialNumberW = to_wstring(
            ConnList->Connections[index].Properties.SerialNumber);
        DWORD DiskNumber = -1;
        HRESULT hres = WvbdGetDiskNumberBySerialNumber(
            SerialNumberW.c_str(), &DiskNumber);
        if (FAILED(hres)) {
            fprintf(stderr,
                    "Warning: Could not retrieve disk number for serial '%ls'. "
                    "HRESULT: 0x%x.\n", SerialNumberW.c_str(), hres);
            Status = HRESULT_CODE(hres);
        }
        printf("%-10d  %-10d %s\n",
               ConnList->Connections[index].Properties.Pid,
               DiskNumber,
               ConnList->Connections[index].Properties.InstanceName);
    }
    free(ConnList);
    return Status;
}

DWORD CmdRaiseLogLevel(UINT32 LogLevel)
{
    DWORD Status = WvbdRaiseLogLevel(LogLevel);
    if (Status && !CheckOpenFailed(Status)) {
        fprintf(stderr, "Could not get connection list.\n");
        PrintFormattedError(Status);
    }
    return Status;
}
