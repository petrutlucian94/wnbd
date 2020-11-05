/*
 * Copyright (c) 2020 SUSE LLC
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#include <windows.h>
#include <windef.h>
#include <winioctl.h>
#include <winioctl.h>
#include <newdev.h>
#include <ntddscsi.h>
#include "Shlwapi.h"
#include <setupapi.h>
#include <string.h>
#include <infstr.h>

#include "wnbd.h"
#include "wnbd_log.h"
#include "utils.h"

#pragma comment(lib, "Setupapi.lib")
#pragma comment(lib, "Newdev.lib")
#pragma comment(lib, "Shlwapi.lib")

#define STRING_OVERFLOWS(Str, MaxLen) (strlen(Str + 1) > MaxLen)

// Open the WNBD SCSI adapter device.
DWORD WnbdOpenAdapterEx(
    PHANDLE Handle,
    HDEVINFO DeviceInfoList,
    PSP_DEVINFO_DATA DeviceInfoData,
    DWORD DevIndex,
    BOOLEAN ExpectExisting)
{
    SP_DEVICE_INTERFACE_DATA DevInterfaceData = { 0 };
    DevInterfaceData.cbSize = sizeof(SP_DEVICE_INTERFACE_DATA);
    PSP_DEVICE_INTERFACE_DETAIL_DATA DevInterfaceDetailData = NULL;
    ULONG RequiredSize = 0;
    ULONG Status = 0;
    HANDLE AdapterHandle = INVALID_HANDLE_VALUE;

    if (!SetupDiEnumDeviceInterfaces(DeviceInfoList, NULL, &WNBD_GUID,
                                     DevIndex, &DevInterfaceData)) {
        Status = GetLastError();
        goto Exit;
    }

    if (!SetupDiGetDeviceInterfaceDetail(DeviceInfoList, &DevInterfaceData, NULL,
                                         0, &RequiredSize, NULL)) {
        Status = GetLastError();
        if (Status && ERROR_INSUFFICIENT_BUFFER != Status) {
            goto Exit;
        }
        else {
            Status = 0;
        }
    }

    DevInterfaceDetailData =
        (PSP_DEVICE_INTERFACE_DETAIL_DATA) malloc(RequiredSize);
    if (!DevInterfaceDetailData) {
        Status = ERROR_BUFFER_OVERFLOW;
        goto Exit;
    }
    DevInterfaceDetailData->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);

    if (!SetupDiGetDeviceInterfaceDetail(
            DeviceInfoList, &DevInterfaceData, DevInterfaceDetailData,
            RequiredSize, &RequiredSize, DeviceInfoData))
    {
        Status = GetLastError();
        goto Exit;
    }

    AdapterHandle = CreateFile(
        DevInterfaceDetailData->DevicePath,
        GENERIC_READ | GENERIC_WRITE, 0, 0, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, 0);
    if (INVALID_HANDLE_VALUE == AdapterHandle) {
        Status = GetLastError();
        goto Exit;
    }

Exit:
    if (DevInterfaceDetailData) {
        free(DevInterfaceDetailData);
    }

    if (!Status) {
        *Handle = AdapterHandle;
    }
    else {
        if (Status == ERROR_ACCESS_DENIED) {
            LogError(
                "Could not open WNBD adapter device. Access denied, try "
                "using an elevated command prompt.");
        } else if (Status == ERROR_NO_MORE_ITEMS) {
            if (ExpectExisting)
                LogError(
                    "No WNBD adapter found. Please make sure that the driver "
                    "is installed.");
        } else {
            LogError(
                "Could not open WNBD adapter device. Please make sure that "
                "the driver is installed. Error: %d. Error message: %s",
                Status, win32_strerror(Status).c_str());
        }
    }
    return Status;
}

// Open the WNBD SCSI adapter device.
DWORD WnbdOpenAdapter(PHANDLE Handle)
{
    DWORD Status = 0;
    SP_DEVINFO_DATA DevInfoData = { 0 };
    DevInfoData.cbSize = sizeof(DevInfoData);

    HDEVINFO DeviceInfoList = SetupDiGetClassDevs(
        &WNBD_GUID, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (DeviceInfoList == INVALID_HANDLE_VALUE) {
        Status = GetLastError();
        LogError(
            "Could not enumerate WNBD adapter devices. "
            "Error: %d. Error message: %s",
            Status, win32_strerror(Status).c_str());
        return Status;
    }

    Status = WnbdOpenAdapterEx(
        Handle, DeviceInfoList, &DevInfoData, 0, TRUE);
    if (!Status) {
        SetupDiDestroyDeviceInfoList(DeviceInfoList);
    }

    return Status;
}

DWORD RemoveWnbdInf(_In_ LPCTSTR InfName)
{
    DWORD Status = 0;
    UINT ErrorLine;

    HINF HandleInf = SetupOpenInfFile(InfName, NULL, INF_STYLE_WIN4, &ErrorLine);
    if (HandleInf == INVALID_HANDLE_VALUE) {
        Status = GetLastError();
        LogError("SetupOpenInfFile failed with "
            "error: %d, at line. Error message: %s",
            ErrorLine, Status, win32_strerror(Status).c_str());
        goto failed;
    }

    INFCONTEXT Context;
    if (!SetupFindFirstLine(HandleInf, INFSTR_SECT_VERSION, INFSTR_KEY_CATALOGFILE, &Context)) {
        Status = GetLastError();
        LogError("SetupFindFirstLine failed with "
            "error: %d. Error message: %s", Status, win32_strerror(Status).c_str());
        goto failed;
    }

    TCHAR InfData[MAX_INF_STRING_LENGTH];
    if (!SetupGetStringField(&Context, 1, InfData, ARRAYSIZE(InfData), NULL)) {
        Status = GetLastError();
        LogError("SetupGetStringField failed with "
            "error: %d. Error message: %s", Status, win32_strerror(Status).c_str());
        goto failed;
    }

    /* Match the OEM inf file based on the catalog string wnbd.cat */
    if (!wcscmp(InfData, L"wnbd.cat")) {
        std::wstring SearchString(InfName);
        if (!SetupUninstallOEMInf(
            SearchString.substr(SearchString.find_last_of(L"\\") + 1).c_str(), SUOI_FORCEDELETE, 0)) {
            Status = GetLastError();
            LogError("SetupUninstallOEMInfA failed with "
                "error: %d. Error message: %s", Status, win32_strerror(Status).c_str());
        }
    }

failed:
    if (HandleInf != INVALID_HANDLE_VALUE) {
        SetupCloseInfFile(HandleInf);
    }

    return Status;
}

/* Cycle through all OEM inf files and try a best effort mode to remove all
 * files which include the string wnbd.cat */
DWORD CleanDrivers()
{
    DWORD Status = 0;
    TCHAR OemName[MAX_PATH];
    HANDLE DirHandle = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATA OemFindData;

    if (!GetWindowsDirectory(OemName, ARRAYSIZE(OemName))) {
        Status = GetLastError();
        LogError("GetWindowsDirectory failed with "
            "error: %d. Error message: %s", Status, win32_strerror(Status).c_str());
        return Status;
    }
    if (wcscat_s(OemName, ARRAYSIZE(OemName), L"\\INF\\OEM*.INF")) {
        LogError("Couldn't process path: %ls.", OemName);
        return ERROR_BAD_PATHNAME;
    }
    DirHandle = FindFirstFile(OemName, &OemFindData);
    if (DirHandle == INVALID_HANDLE_VALUE) {
        /* No OEM files */
        return Status;
    }

    do {
        Status = RemoveWnbdInf(OemFindData.cFileName);
        if (Status) {
            LogError("Failed while trying to remove OEM file: %ls",
                OemFindData.cFileName);
            break;
        }
    } while (FindNextFile(DirHandle, &OemFindData));

    FindClose(DirHandle);
    return Status;
}

DWORD RemoveWnbdAdapterDevice(HDEVINFO DeviceInfoList, PSP_DEVINFO_DATA DevInfoData, PBOOL RebootRequired)
{
    SP_DRVINFO_DETAIL_DATA_A DrvDetailData = { 0 };
    DrvDetailData.cbSize = sizeof DrvDetailData;
    DWORD Status = ERROR_SUCCESS;

    /* Queue the device for removal before trying to remove the OEM information file */
    if (!DiUninstallDevice(0, DeviceInfoList, DevInfoData, 0, RebootRequired)) {
        Status = GetLastError();
        LogError(
            "Could not remove driver. "
            "Error: %d. Error message: %s", Status, win32_strerror(Status).c_str());
    }

    return Status;
}

DWORD RemoveAllWnbdDisks(HANDLE AdapterHandle)
{
    DWORD BufferSize = 0;
    DWORD Status = 0;
    PWNBD_CONNECTION_LIST ConnList = NULL;

    WNBD_OPTION_VALUE OptValue = { WnbdOptBool };
    /* Disallow new mappings so we can remove all current mappings */
    OptValue.Data.AsBool = FALSE;
    Status = WnbdIoctlSetDrvOpt(
        AdapterHandle, "NewMappingsAllowed", &OptValue, FALSE, NULL);
    if (Status) {
        goto exit;
    }

    Status = WnbdIoctlList(AdapterHandle, ConnList, &BufferSize, NULL);
    if (!BufferSize) {
        goto exit;
    }
    ConnList = (PWNBD_CONNECTION_LIST)calloc(1, BufferSize);
    if (NULL == ConnList) {
        Status = ERROR_NOT_ENOUGH_MEMORY;
        LogError(
            "Failed to allocate %d bytes.", BufferSize);
        goto exit;
    }
    Status = WnbdIoctlList(AdapterHandle, ConnList, &BufferSize, NULL);
    if (Status) {
        goto exit;
    }
    for (ULONG index = 0; index < ConnList->Count; index++) {
        char* InstanceName = ConnList->Connections[index].Properties.InstanceName;
        /* TODO add parallel and soft disconnect removal */
        Status = WnbdIoctlRemove(AdapterHandle, InstanceName, NULL, NULL);
        if (Status) {
            goto exit;
        }
    }

exit:
    if (ConnList) {
        free(ConnList);
    }
    return Status;
}

// Remove all WNBD adapters and disks.
DWORD RemoveAllWnbdDevices(PBOOL RebootRequired) {
    DWORD Status = 0, Status2 = 0;
    DWORD DevIndex = 0;

    HDEVINFO DeviceInfoList = SetupDiGetClassDevs(
        &WNBD_GUID, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (DeviceInfoList == INVALID_HANDLE_VALUE) {
        Status = GetLastError();
        LogError(
            "Could not enumerate WNBD adapter devices. "
            "Error: %d. Error message: %s",
            Status, win32_strerror(Status).c_str());
        return Status;
    }

    // Iterate over all WNBD adapters and remove any associated disk.
    // In case of failure, we proceed to the next adapter and
    // return the first encountered error at the end.
    while (TRUE) {
        HANDLE AdapterHandle = INVALID_HANDLE_VALUE;
        SP_DEVINFO_DATA DevInfoData;
        DevInfoData.cbSize = sizeof DevInfoData;

        DWORD Status = WnbdOpenAdapterEx(
            &AdapterHandle, &DeviceInfoList, &DevInfoData, DevIndex++, FALSE);
        if (Status) {
            Status = Status == ERROR_NO_MORE_ITEMS ? 0 : Status;
            break;
        }

        Status2 = RemoveAllWnbdDisks(AdapterHandle);
        Status = Status ? Status : Status2;

        Status2 = RemoveWnbdAdapterDevice(DeviceInfoList, &DevInfoData, RebootRequired);
        Status = Status ? Status : Status2;

        CloseHandle(AdapterHandle);
        SetupDiDestroyDeviceInfoList(DeviceInfoList);
    }

    return Status;
}

DWORD WnbdUninstallDriver(PBOOL RebootRequired)
{
    DWORD Status = RemoveAllWnbdDevices(RebootRequired);
    DWORD Status2 = CleanDrivers();
    return Status ? Status : Status2;
}

DWORD CreateWnbdAdapter(CHAR* ClassName, SP_DEVINFO_DATA* DevInfoData, HDEVINFO* DeviceInfoList)
{
    DWORD Status = 0;

    if (INVALID_HANDLE_VALUE == (*DeviceInfoList = SetupDiCreateDeviceInfoList(&WNBD_GUID, 0))) {
        Status = GetLastError();
        LogError(
            "SetupDiCreateDeviceInfoList failed with error: %d. Error message: %s",
            Status, win32_strerror(Status).c_str());
        return Status;
    }

    if (!SetupDiCreateDeviceInfoA(*DeviceInfoList, ClassName, &WNBD_GUID, 0, 0, DICD_GENERATE_ID, DevInfoData)) {
        Status = GetLastError();
        LogError(
            "SetupDiCreateDeviceInfoA failed with error: %d. Error message: %s",
            Status, win32_strerror(Status).c_str());
        return Status;
    }

    if (!SetupDiSetDeviceRegistryPropertyA(*DeviceInfoList, DevInfoData,
        SPDRP_HARDWAREID, (PBYTE)WNBD_HARDWAREID, WNBD_HARDWAREID_LEN)) {
        Status = GetLastError();
        LogError(
            "SetupDiSetDeviceRegistryPropertyA failed with error: %d. Error message: %s",
            Status, win32_strerror(Status).c_str());
        return Status;
    }

    return Status;
}

DWORD InstallDriver(CHAR* FileNameBuf, HDEVINFO* DeviceInfoList, PBOOL RebootRequired)
{
    GUID ClassGuid = { 0 };
    CHAR ClassName[MAX_CLASS_NAME_LEN];
    SP_DEVINFO_DATA DevInfoData;
    DevInfoData.cbSize = sizeof(DevInfoData);
    SP_DEVINSTALL_PARAMS_A InstallParams;
    DWORD Status = 0;

    if (!SetupDiGetINFClassA(FileNameBuf, &ClassGuid, ClassName, MAX_CLASS_NAME_LEN, 0)) {
        Status = GetLastError();
        LogError(
            "SetupDiGetINFClassA failed with error: %d. Error message: %s",
            Status, win32_strerror(Status).c_str());
        return Status;
    }

    if (CreateWnbdAdapter(ClassName, &DevInfoData, DeviceInfoList)) {
        return Status;
    }

    if (!SetupDiCallClassInstaller(DIF_REGISTERDEVICE, *DeviceInfoList, &DevInfoData)) {
        Status = GetLastError();
        LogError(
            "SetupDiCallClassInstaller failed with error: %d. Error message: %s",
            Status, win32_strerror(Status).c_str());
        return Status;
    }

    InstallParams.cbSize = sizeof InstallParams;
    if (!SetupDiGetDeviceInstallParamsA(*DeviceInfoList, &DevInfoData, &InstallParams)) {
        Status = GetLastError();
        LogError(
            "SetupDiGetDeviceInstallParamsA failed with error: %d. Error message: %s",
            Status, win32_strerror(Status).c_str());
        return Status;
    }

    if (0 != (InstallParams.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART))) {
        *RebootRequired = TRUE;
    }

    return Status;
}

DWORD WnbdInstallDriver(CONST CHAR* FileName, PBOOL RebootRequired)
{
    CHAR FullFileName[MAX_PATH];
    DWORD Status = ERROR_SUCCESS;
    HDEVINFO DeviceInfoList = INVALID_HANDLE_VALUE;
    SP_DEVINFO_DATA DevInfoData;
    DevInfoData.cbSize = sizeof DevInfoData;
    HANDLE AdapterHandle = INVALID_HANDLE_VALUE;

    if (0 == GetFullPathNameA(FileName, MAX_PATH, FullFileName, 0)) {
        Status = GetLastError();
        LogError(
            "Invalid file path: %s. Error: %d. Error message: %s",
            FileName, Status, win32_strerror(Status).c_str());
        goto exit;
    }
    if (FALSE == PathFileExistsA(FullFileName)) {
        LogError("Could not find file: %s.", FullFileName);
        Status = ERROR_FILE_NOT_FOUND;
        goto exit;
    }

    Status = WnbdOpenAdapter(&AdapterHandle);
    if (!Status) {
        LogError("A WNBD adapter already exists. Please uninstall it "
                 "before updating the driver.");
        Status = ERROR_DUPLICATE_FOUND;
        CloseHandle(AdapterHandle);
        goto exit;
    }

    Status = InstallDriver(FullFileName, &DeviceInfoList, RebootRequired);
    if (ERROR_SUCCESS != Status) {
        LogError(
            "Failed to install driver. Error: %d. Error message: %s",
            Status, win32_strerror(Status).c_str());
        goto exit;
    }

    if (!UpdateDriverForPlugAndPlayDevicesA(0, WNBD_HARDWAREID, FullFileName, 0, RebootRequired)) {
        Status = GetLastError();
        LogError(
            "UpdateDriverForPlugAndPlayDevicesA failed with error: %d. Error message: %s",
            Status, win32_strerror(Status).c_str());
        goto exit;
    }

exit:
    return Status;
}

DWORD WnbdDeviceIoControl(
    HANDLE hDevice,
    DWORD dwIoControlCode,
    LPVOID lpInBuffer,
    DWORD nInBufferSize,
    LPVOID lpOutBuffer,
    DWORD nOutBufferSize,
    LPDWORD lpBytesReturned,
    LPOVERLAPPED lpOverlapped)
{
    OVERLAPPED InternalOverlapped = { 0 };
    DWORD Status = ERROR_SUCCESS;
    HANDLE TempEvent = NULL;

    // DeviceIoControl can hang when FILE_FLAG_OVERLAPPED is used
    // without a valid overlapped structure. We're providing one and also
    // do the wait on behalf of the caller when lpOverlapped is NULL,
    // mimicking the Windows API.
    if (!lpOverlapped) {
        TempEvent = CreateEventA(0, FALSE, FALSE, NULL);
        if (!TempEvent) {
            Status = GetLastError();
            LogError("Could not create event. Error: %d. Error message: %s",
                     Status, win32_strerror(Status).c_str());
            return Status;
        }
        InternalOverlapped.hEvent = TempEvent;
        lpOverlapped = &InternalOverlapped;
    }

    BOOL DevStatus = DeviceIoControl(
        hDevice, dwIoControlCode, lpInBuffer, nInBufferSize, lpOutBuffer,
        nOutBufferSize, lpBytesReturned, lpOverlapped);
    if (!DevStatus) {
        Status = GetLastError();
        if (Status == ERROR_IO_PENDING && TempEvent) {
            // We might consider an alertable wait using GetOverlappedResultEx.
            if (!GetOverlappedResult(hDevice, lpOverlapped,
                                     lpBytesReturned, TRUE)) {
                Status = GetLastError();
            }
            else {
                // The asynchronous operation completed successfully.
                Status = 0;
            }
        }
    }

    if (TempEvent) {
        CloseHandle(TempEvent);
    }

    return Status;
}

DWORD WnbdIoctlCreate(
    HANDLE Adapter,
    PWNBD_PROPERTIES Properties,
    PWNBD_CONNECTION_INFO ConnectionInfo,
    LPOVERLAPPED Overlapped)
{
    DWORD Status = ERROR_SUCCESS;

    // Perform some minimal input validation before passing the request.
    if (STRING_OVERFLOWS(Properties->InstanceName, WNBD_MAX_NAME_LENGTH) ||
        STRING_OVERFLOWS(Properties->SerialNumber, WNBD_MAX_NAME_LENGTH) ||
        STRING_OVERFLOWS(Properties->Owner, WNBD_MAX_OWNER_LENGTH) ||
        STRING_OVERFLOWS(Properties->NbdProperties.Hostname, WNBD_MAX_NAME_LENGTH) ||
        STRING_OVERFLOWS(Properties->NbdProperties.ExportName, WNBD_MAX_NAME_LENGTH))
    {
        LogError("Invalid WNBD properties. Buffer overflow.");
        return ERROR_BUFFER_OVERFLOW;
    }

    if (!Properties->InstanceName) {
        LogError("Missing instance name.");
        return ERROR_INVALID_PARAMETER;
    }

    DWORD BytesReturned = 0;
    WNBD_IOCTL_CREATE_COMMAND Command = { 0 };

    Command.IoControlCode = IOCTL_WNBD_CREATE;
    memcpy(&Command.Properties, Properties, sizeof(WNBD_PROPERTIES));

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command), ConnectionInfo, sizeof(WNBD_CONNECTION_INFO),
        &BytesReturned, Overlapped);
    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        LogError("Could not create WNBD disk. Error: %d. Error message: %s",
                 Status, win32_strerror(Status).c_str());
    }

    return Status;
}

DWORD WnbdIoctlRemove(
    HANDLE Adapter, const char* InstanceName,
    PWNBD_REMOVE_COMMAND_OPTIONS RemoveOptions,
    LPOVERLAPPED Overlapped)
{
    DWORD Status = ERROR_SUCCESS;

    if (STRING_OVERFLOWS(InstanceName, WNBD_MAX_NAME_LENGTH)) {
        LogError("Invalid instance name. Buffer overflow.");
        return ERROR_BUFFER_OVERFLOW;
    }

    if (!InstanceName) {
        LogError("Missing instance name.");
        return ERROR_INVALID_PARAMETER;
    }

    DWORD BytesReturned = 0;
    WNBD_IOCTL_REMOVE_COMMAND Command = { 0 };

    Command.IoControlCode = IOCTL_WNBD_REMOVE;
    if (RemoveOptions) {
        Command.Options = *RemoveOptions;
    }
    memcpy(Command.InstanceName, InstanceName, strlen(InstanceName));

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command), NULL, 0, &BytesReturned, Overlapped);

    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        if (Status == ERROR_FILE_NOT_FOUND) {
            LogDebug("Could not find the disk to be removed.");
        }
        else {
            LogError("Could not remove WNBD disk. Error: %d. Error message: %s",
                     Status, win32_strerror(Status).c_str());
        }
    }
    return Status;
}

DWORD WnbdIoctlFetchRequest(
    HANDLE Adapter,
    WNBD_CONNECTION_ID ConnectionId,
    PWNBD_IO_REQUEST Request,
    PVOID DataBuffer,
    UINT32 DataBufferSize,
    LPOVERLAPPED Overlapped)
{
    DWORD Status = ERROR_SUCCESS;

    DWORD BytesReturned = 0;
    WNBD_IOCTL_FETCH_REQ_COMMAND Command = { 0 };

    Command.IoControlCode = IOCTL_WNBD_FETCH_REQ;
    Command.ConnectionId = ConnectionId;
    Command.DataBuffer = DataBuffer;
    Command.DataBufferSize = DataBufferSize;

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(WNBD_IOCTL_FETCH_REQ_COMMAND),
        &Command, sizeof(WNBD_IOCTL_FETCH_REQ_COMMAND),
        &BytesReturned, Overlapped);

    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        LogWarning(
            "Could not fetch request. Error: %d. "
            "Buffer: %p, buffer size: %d, connection id: %llu. "
            "Error message: %s",
            Status, DataBuffer, DataBufferSize, ConnectionId,
            win32_strerror(Status).c_str());
    }
    else {
        memcpy(Request, &Command.Request, sizeof(WNBD_IO_REQUEST));
    }

    return Status;
}

DWORD WnbdIoctlSendResponse(
    HANDLE Adapter,
    WNBD_CONNECTION_ID ConnectionId,
    PWNBD_IO_RESPONSE Response,
    PVOID DataBuffer,
    UINT32 DataBufferSize,
    LPOVERLAPPED Overlapped)
{
    DWORD Status = ERROR_SUCCESS;

    DWORD BytesReturned = 0;
    WNBD_IOCTL_SEND_RSP_COMMAND Command = { 0 };

    Command.IoControlCode = IOCTL_WNBD_SEND_RSP;
    Command.ConnectionId = ConnectionId;
    Command.DataBuffer = DataBuffer;
    Command.DataBufferSize = DataBufferSize;
    memcpy(&Command.Response, Response, sizeof(WNBD_IO_RESPONSE));

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(WNBD_IOCTL_SEND_RSP_COMMAND),
        NULL, 0, &BytesReturned, Overlapped);

    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        LogWarning(
            "Could not send response. "
            "Connection id: %llu. Request id: %llu. "
            "Error: %d. Error message: %s",
            ConnectionId, Response->RequestHandle,
            Status, win32_strerror(Status).c_str());
    }

    return Status;
}

DWORD WnbdIoctlList(
    HANDLE Adapter,
    PWNBD_CONNECTION_LIST ConnectionList,
    PDWORD BufferSize,
    LPOVERLAPPED Overlapped)
{
    DWORD Status = ERROR_SUCCESS;
    WNBD_IOCTL_LIST_COMMAND Command = { IOCTL_WNBD_LIST };

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command), ConnectionList,
        *BufferSize, BufferSize, Overlapped);

    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        LogError("Could not get disk list. Error: %d. Error message: %s",
                 Status, win32_strerror(Status).c_str());
    }

    return Status;
}

DWORD WnbdIoctlStats(
    HANDLE Adapter,
    const char* InstanceName,
    PWNBD_DRV_STATS Stats,
    LPOVERLAPPED Overlapped)
{
    DWORD Status = ERROR_SUCCESS;
    DWORD BytesReturned = 0;

    WNBD_IOCTL_STATS_COMMAND Command = { 0 };
    Command.IoControlCode = IOCTL_WNBD_STATS;
    memcpy(Command.InstanceName, InstanceName, strlen(InstanceName));

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command),
        Stats, sizeof(WNBD_DRV_STATS), &BytesReturned, Overlapped);

    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        if (Status == ERROR_FILE_NOT_FOUND) {
            LogInfo("Could not find the specified disk.");
        }
        else {
            LogError("Could not get disk stats. Error: %d. Error message: %s",
                     Status, win32_strerror(Status).c_str());
        }
    }

    return Status;
}

DWORD WnbdIoctlShow(
    HANDLE Adapter,
    const char* InstanceName,
    PWNBD_CONNECTION_INFO ConnectionInfo,
    LPOVERLAPPED Overlapped)
{
    DWORD Status = ERROR_SUCCESS;
    DWORD BytesReturned = 0;

    WNBD_IOCTL_SHOW_COMMAND Command = { 0 };
    Command.IoControlCode = IOCTL_WNBD_SHOW;
    memcpy(Command.InstanceName, InstanceName, strlen(InstanceName));

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command),
        ConnectionInfo, sizeof(WNBD_CONNECTION_INFO),
        &BytesReturned, Overlapped);

    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        if (Status == ERROR_FILE_NOT_FOUND) {
            LogInfo("Could not find the specified disk.");
        }
        else {
            LogError("Could not get disk details. Error: %d. Error message: %s",
                     Status, win32_strerror(Status).c_str());
        }
    }

    return Status;
}

DWORD WnbdIoctlReloadConfig(
    HANDLE Adapter,
    LPOVERLAPPED Overlapped)
{
    DWORD BytesReturned = 0;
    DWORD Status = ERROR_SUCCESS;
    WNBD_IOCTL_RELOAD_CONFIG_COMMAND Command = { IOCTL_WNBD_RELOAD_CONFIG };

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command), NULL, 0, &BytesReturned, Overlapped);

    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        LogError("Could not get reload driver config. "
                 "Error: %d. Error message: %s",
                 Status, win32_strerror(Status).c_str());
    }

    return Status;
}

DWORD WnbdIoctlPing(
    HANDLE Adapter,
    LPOVERLAPPED Overlapped)
{
    DWORD BytesReturned = 0;
    DWORD Status = ERROR_SUCCESS;
    WNBD_IOCTL_PING_COMMAND Command = { IOCTL_WNBD_PING };

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command), NULL, 0, &BytesReturned, Overlapped);

    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        LogError("Failed while pinging driver. "
                 "Error: %d. Error message: %s",
                 Status, win32_strerror(Status).c_str());
    }

    return Status;
}

DWORD WnbdIoctlGetDrvOpt(
    HANDLE Adapter,
    const char* Name,
    PWNBD_OPTION_VALUE Value,
    BOOLEAN Persistent,
    LPOVERLAPPED Overlapped)
{
    DWORD Status = ERROR_SUCCESS;
    DWORD BytesReturned = 0;

    WNBD_IOCTL_GET_DRV_OPT_COMMAND Command = { 0 };
    Command.IoControlCode = IOCTL_WNBD_GET_DRV_OPT;
    Command.Persistent = Persistent;
    to_wstring(Name).copy(Command.Name, WNBD_MAX_OPT_NAME_LENGTH);

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command),
        Value, sizeof(WNBD_OPTION_VALUE),
        &BytesReturned, Overlapped);

    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        if (Status == ERROR_FILE_NOT_FOUND) {
            LogError("Could not find driver option: %s.", Name);
        }
        else {
            LogError("Could not get adapter option. "
                     "Error: %d. Error message: %s",
                     Status, win32_strerror(Status).c_str());
        }
    }

    return Status;
}

DWORD WnbdIoctlSetDrvOpt(
    HANDLE Adapter,
    const char* Name,
    PWNBD_OPTION_VALUE Value,
    BOOLEAN Persistent,
    LPOVERLAPPED Overlapped)
{
    DWORD Status = ERROR_SUCCESS;
    DWORD BytesReturned = 0;

    WNBD_IOCTL_SET_DRV_OPT_COMMAND Command = { 0 };
    Command.IoControlCode = IOCTL_WNBD_SET_DRV_OPT;
    Command.Persistent = Persistent;
    Command.Value = *Value;
    to_wstring(Name).copy(Command.Name, WNBD_MAX_OPT_NAME_LENGTH);

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command),
        NULL, 0,
        &BytesReturned, Overlapped);

    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        if (Status == ERROR_FILE_NOT_FOUND) {
            LogError("Could not find driver option: %s.", Name);
        }
        else {
            LogError("Could not set adapter option. "
                     "Error: %d. Error message: %s",
                     Status, win32_strerror(Status).c_str());
        }
    }

    return Status;
}

DWORD WnbdIoctlResetDrvOpt(
    HANDLE Adapter,
    const char* Name,
    BOOLEAN Persistent,
    LPOVERLAPPED Overlapped)
{
    DWORD Status = ERROR_SUCCESS;
    DWORD BytesReturned = 0;

    WNBD_IOCTL_RESET_DRV_OPT_COMMAND Command = { 0 };
    Command.IoControlCode = IOCTL_WNBD_RESET_DRV_OPT;
    Command.Persistent = Persistent;
    to_wstring(Name).copy(Command.Name, WNBD_MAX_OPT_NAME_LENGTH);

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command),
        NULL, 0,
        &BytesReturned, Overlapped);

    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        if (Status == ERROR_FILE_NOT_FOUND) {
            LogError("Could not find driver option: %s.", Name);
        }
        else {
            LogError("Could not reset adapter option. "
                     "Error: %d. Error message: %s",
                     Status, win32_strerror(Status).c_str());
        }
    }

    return Status;
}

DWORD WnbdIoctlListDrvOpt(
    HANDLE Adapter,
    PWNBD_OPTION_LIST OptionList,
    PDWORD BufferSize,
    BOOLEAN Persistent,
    LPOVERLAPPED Overlapped)
{
    DWORD Status = ERROR_SUCCESS;
    WNBD_IOCTL_LIST_DRV_OPT_COMMAND Command = { 0 };
    Command.IoControlCode = IOCTL_WNBD_LIST_DRV_OPT;
    Command.Persistent = Persistent;

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command), OptionList,
        *BufferSize, BufferSize, Overlapped);

    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        LogError("Could not get option list. "
                 "Error: %d. Error message: %s",
                 Status, win32_strerror(Status).c_str());
    }

    return Status;
}

DWORD WnbdIoctlVersion(
    HANDLE Adapter,
    PWNBD_VERSION Version,
    LPOVERLAPPED Overlapped)
{
    DWORD Status = ERROR_SUCCESS;
    DWORD BytesReturned = 0;

    WNBD_IOCTL_VERSION_COMMAND Command = { 0 };
    Command.IoControlCode = IOCTL_WNBD_VERSION;

    Status = WnbdDeviceIoControl(
        Adapter, IOCTL_MINIPORT_PROCESS_SERVICE_IRP,
        &Command, sizeof(Command),
        Version, sizeof(WNBD_VERSION),
        &BytesReturned, Overlapped);

    if (Status && !(Status == ERROR_IO_PENDING && Overlapped)) {
        LogError("Could not get driver version. "
                 "Error: %d. Error message: %s",
                 Status, win32_strerror(Status).c_str());
    }

    return Status;
}
