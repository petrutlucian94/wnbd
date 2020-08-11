#include <windows.h>
#include <windef.h>
#include <winioctl.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <setupapi.h>
#include <string.h>

#include "wvbd.h"

#pragma comment(lib, "Setupapi.lib")

HANDLE WvbdOpenDevice()
{
    HDEVINFO DevInfo = { 0 };
    SP_DEVICE_INTERFACE_DATA DevInterfaceData = { 0 };
    PSP_DEVICE_INTERFACE_DETAIL_DATA DevInterfaceDetailData = NULL;
    ULONG DevIndex = 0;
    ULONG RequiredSize = 0;
    ULONG ErrorCode = 0;
    HANDLE WnbdDriverHandle = INVALID_HANDLE_VALUE;
    DWORD BytesReturned = 0;
    WNBD_COMMAND Command = { 0 };
    BOOL DevStatus = 0;

    DevInfo = SetupDiGetClassDevs(&WNBD_GUID, NULL, NULL,
                                  DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (DevInfo == INVALID_HANDLE_VALUE) {
        goto Exit;
    }

    DevInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    DevIndex = 0;

    while (SetupDiEnumDeviceInterfaces(DevInfo, NULL, &WNBD_GUID,
                                       DevIndex++, &DevInterfaceData)) {
        if (!SetupDiGetDeviceInterfaceDetail(DevInfo, &DevInterfaceData, NULL,
                                             0, &RequiredSize, NULL)) {
            ErrorCode = GetLastError();
            if (ERROR_INSUFFICIENT_BUFFER != ErrorCode) {
                goto Exit;
            }
        }

        DevInterfaceDetailData =
          (PSP_DEVICE_INTERFACE_DETAIL_DATA)malloc(RequiredSize);

        if (!DevInterfaceDetailData) {
            goto Exit;
        }

        DevInterfaceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

        if (!SetupDiGetDeviceInterfaceDetail(
              DevInfo, &DevInterfaceData, DevInterfaceDetailData,
              RequiredSize, &RequiredSize, NULL)) {
            goto Exit;
        }

        WnbdDriverHandle = CreateFile(
            DevInterfaceDetailData->DevicePath,
            GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, 0);

        Command.IoCode = IOCTL_WNBD_PORT;

        DevStatus = DeviceIoControl(
            WnbdDriverHandle, IOCTL_MINIPORT_PROCESS_SERVICE_IRP, &Command,
            sizeof(Command), &Command, sizeof(Command), &BytesReturned, NULL);

        if (!DevStatus) {
            ErrorCode = GetLastError();
            CloseHandle(WnbdDriverHandle);
            WnbdDriverHandle = INVALID_HANDLE_VALUE;
            continue;
        } else {
            goto Exit;
        }
    }

    ErrorCode = GetLastError();
    if (ErrorCode != ERROR_NO_MORE_ITEMS) {
        goto Exit;
    }

Exit:
    if (DevInterfaceDetailData) {
        free(DevInterfaceDetailData);
    }
    if (DevInfo) {
        SetupDiDestroyDeviceInfoList(DevInfo);
    }

    return WnbdDriverHandle;
}

DWORD WvbdIoctlCreate(HANDLE Device, PWVBD_PROPERTIES Properties,
                      PWVBD_DISK_HANDLE DiskHandle)
{
    DWORD ErrorCode = ERROR_SUCCESS;

    if (STRING_OVERFLOWS(Properties->InstanceName, MAX_NAME_LENGTH)) {
        return ERROR_BUFFER_OVERFLOW;
    }

    BOOL DevStatus = 0;
    DWORD BytesReturned = 0;
    WVBD_IOCTL_CREATE_COMMAND Command = { 0 };

    Command.IoControlCode = IOCTL_WVBD_CREATE;
    memcpy(&Command.Properties, Properties, sizeof(WVBD_PROPERTIES));

    DevStatus = DeviceIoControl(
        Device, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command), &DiskHandle, sizeof(WVBD_DISK_HANDLE),
        &BytesReturned, NULL);

    if (!DevStatus) {
        ErrorCode = GetLastError();
    }

    return ErrorCode;
}


DWORD WvbdIoctlRemove(HANDLE Device, PCHAR InstanceName)
{
    DWORD Status = ERROR_SUCCESS;

    if (STRING_OVERFLOWS(InstanceName, MAX_NAME_LENGTH)) {
        return ERROR_BUFFER_OVERFLOW;
    }

    BOOL DevStatus = 0;
    DWORD BytesReturned = 0;
    WVBD_IOCTL_REMOVE_COMMAND Command = { 0 };

    Command.IoControlCode = IOCTL_WVBD_REMOVE;
    memcpy(Command.InstanceName, InstanceName, strlen(InstanceName));

    DevStatus = DeviceIoControl(
        Device, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command), NULL, 0, &BytesReturned, NULL);

    if (!DevStatus) {
        Status = GetLastError();
    }

    return Status;
}

DWORD WvbdIoctlFetchRequest(
    HANDLE Device,
    WVBD_DISK_HANDLE DiskHandle,
    PWVBD_IO_REQUEST Request,
    PVOID DataBuffer,
    UINT32 DataBufferSize)
{
    DWORD Status = ERROR_SUCCESS;

    BOOL DevStatus = 0;
    DWORD BytesReturned = 0;
    WVBD_IOCTL_FETCH_REQ_COMMAND Command = { 0 };

    Command.IoControlCode = IOCTL_WVBD_FETCH_REQ;
    Command.DiskHandle = DiskHandle;
    Command.DataBuffer = DataBuffer;
    Command.DataBufferSize = DataBufferSize;

    DevStatus = DeviceIoControl(
        Device, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(WVBD_IOCTL_FETCH_REQ_COMMAND),
        &Command, sizeof(WVBD_IOCTL_FETCH_REQ_COMMAND),
        &BytesReturned, NULL);

    if (!DevStatus) {
        Status = GetLastError();
    }
    else {
        memcpy(Request, &Command.Request, sizeof(WVBD_IO_REQUEST));
    }

    return Status;
}

DWORD WvbdIoctlSendResponse(
    HANDLE Device,
    WVBD_DISK_HANDLE DiskHandle,
    PWVBD_IO_RESPONSE Response,
    PVOID DataBuffer,
    UINT32 DataBufferSize)
{
    DWORD Status = ERROR_SUCCESS;

    BOOL DevStatus = 0;
    DWORD BytesReturned = 0;
    WVBD_IOCTL_SEND_RSP_COMMAND Command = { 0 };

    Command.IoControlCode = IOCTL_WVBD_SEND_RSP;
    Command.DiskHandle = DiskHandle;
    Command.DataBuffer = DataBuffer;
    Command.DataBufferSize = DataBufferSize;
    memcpy(&Command.Response, Response, sizeof(WVBD_IO_RESPONSE));

    DevStatus = DeviceIoControl(
        Device, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(WVBD_IOCTL_SEND_RSP_COMMAND),
        NULL, 0, &BytesReturned, NULL);

    if (!DevStatus) {
        Status = GetLastError();
    }

    return Status;
}
