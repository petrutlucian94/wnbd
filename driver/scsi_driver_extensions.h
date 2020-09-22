/*
 * Copyright (c) 2019 SUSE LLC
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#ifndef SCSI_DRIVER_EXTENSIONS_H
#define SCSI_DRIVER_EXTENSIONS_H 1

#include "common.h"

typedef struct _WNBD_EXTENSION {
    SCSI_ADAPTER_CONTROL_TYPE         ScsiAdapterControlState;

    UNICODE_STRING                    DeviceInterface;
    LIST_ENTRY                        DeviceList;
    KSPIN_LOCK                        DeviceListLock;
    LONG                             DeviceCount;
    ERESOURCE                         DeviceResourceLock;

    HANDLE                            DeviceCleaner;
    KEVENT                            DeviceCleanerEvent;
    BOOLEAN                           StopDeviceCleaner;
} WNBD_EXTENSION, *PWNBD_EXTENSION;

SCSI_ADAPTER_CONTROL_STATUS
WnbdHwAdapterControl(_In_ PVOID DeviceExtension,
                     _In_ SCSI_ADAPTER_CONTROL_TYPE ControlType,
                     _In_ PVOID Parameters);

ULONG
WnbdHwFindAdapter(_In_ PVOID DeviceExtension,
                  _In_ PVOID HwContext,
                  _In_ PVOID BusInformation,
                  _In_ PVOID LowerDevice,
                  _In_ PCHAR ArgumentString,
                  _Inout_ PPORT_CONFIGURATION_INFORMATION ConfigInfo,
                  _In_ PBOOLEAN Again);

VOID
WnbdHwFreeAdapterResources(_In_ PVOID DeviceExtension);


BOOLEAN
WnbdHwInitialize(_In_ PVOID DeviceExtension);

VOID
WnbdHwProcessServiceRequest(_In_ PVOID DeviceExtension,
                            _In_ PVOID Irp);

BOOLEAN
WnbdHwResetBus(_In_ PVOID DeviceExtension,
               _In_ ULONG PathId);

BOOLEAN
WnbdHwStartIo(_In_ PVOID PDevExt,
              _In_ PSCSI_REQUEST_BLOCK  PSrb);

#endif
