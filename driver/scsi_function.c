/*
 * Copyright (c) 2019 SUSE LLC
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#include "common.h"
#include "debug.h"
#include "scsi_driver_extensions.h"
#include "scsi_operation.h"
#include "scsi_trace.h"
#include "srb_helper.h"
#include "scsi_function.h"
#include "util.h"
#include "userspace.h"

UCHAR DrainDeviceQueues(PVOID DeviceExtension,
                        PSCSI_REQUEST_BLOCK Srb)

{
    WNBD_LOG_LOUD(": Enter");
    ASSERT(Srb);
    ASSERT(DeviceExtension);

    UCHAR SrbStatus = SRB_STATUS_NO_DEVICE;
    PWNBD_SCSI_DEVICE Device;
    PWNBD_LU_EXTENSION LuExtension;
    PWNBD_EXTENSION DevExtension = (PWNBD_EXTENSION)DeviceExtension;
    KIRQL Irql;
    KSPIN_LOCK DevLock = DevExtension->DeviceListLock;
    KeAcquireSpinLock(&DevLock, &Irql);

    if (SrbGetCdb(Srb)) {
        BYTE CdbValue = SrbGetCdb(Srb)->AsByte[0];

        WNBD_LOG_INFO(": Received %#02x command. SRB = 0x%p. CDB = 0x%x. PathId: %d TargetId: %d LUN: %d",
            CdbValue, Srb, CdbValue, Srb->PathId, Srb->TargetId, Srb->Lun);
    }

    LuExtension = (PWNBD_LU_EXTENSION)
        StorPortGetLogicalUnit(DeviceExtension, Srb->PathId, Srb->TargetId, Srb->Lun);

    if (!LuExtension) {
        WNBD_LOG_ERROR(": Unable to get LUN extension for device PathId: %d TargetId: %d LUN: %d",
            Srb->PathId, Srb->TargetId, Srb->Lun);
        goto Exit;
    }

    Device = WnbdFindDevice(LuExtension, DeviceExtension,
                            Srb->PathId, Srb->TargetId, Srb->Lun);
    if (NULL == Device) {
        WNBD_LOG_INFO("Could not find device PathId: %d TargetId: %d LUN: %d",
            Srb->PathId, Srb->TargetId, Srb->Lun);
        goto Exit;
    }

    if (NULL == Device->ScsiDeviceExtension) {
        WNBD_LOG_ERROR("%p has no ScsiDeviceExtension. PathId = %d. TargetId = %d. LUN = %d",
            Device, Srb->PathId, Srb->TargetId, Srb->Lun);
        goto Exit;
    }
    if (Device->Missing) {
        WNBD_LOG_WARN("%p is marked for deletion. PathId = %d. TargetId = %d. LUN = %d",
            Device, Srb->PathId, Srb->TargetId, Srb->Lun);
        /// Drain the queue here because the device doesn't theoretically exist;
        DrainDeviceQueue(WNBD_DEV_INFO(Device), FALSE);
        DrainDeviceQueue(WNBD_DEV_INFO(Device), TRUE);
        goto Exit;
    }

    DrainDeviceQueue(WNBD_DEV_INFO(Device), FALSE);
    AbortSubmittedRequests(WNBD_DEV_INFO(Device));

    SrbStatus = SRB_STATUS_SUCCESS;

Exit:
    KeReleaseSpinLock(&DevLock, Irql);

    WNBD_LOG_LOUD(": Exit");
    return SrbStatus;
}

_Use_decl_annotations_
UCHAR
WnbdAbortFunction(_In_ PVOID DeviceExtension,
                  _In_ PSCSI_REQUEST_BLOCK Srb)
{
    WNBD_LOG_LOUD(": Enter");

    ASSERT(Srb);
    ASSERT(DeviceExtension);

    UCHAR SrbStatus = DrainDeviceQueues(DeviceExtension, Srb);

    WNBD_LOG_LOUD(": Exit");

    return SrbStatus;
}

_Use_decl_annotations_
UCHAR
WnbdResetLogicalUnitFunction(PVOID DeviceExtension,
                             PSCSI_REQUEST_BLOCK Srb)
{
    WNBD_LOG_LOUD(": Enter");

    ASSERT(Srb);
    ASSERT(DeviceExtension);

    UCHAR SrbStatus = DrainDeviceQueues(DeviceExtension, Srb);

    WNBD_LOG_LOUD(": Exit");

    return SrbStatus;
}

_Use_decl_annotations_
UCHAR
WnbdResetDeviceFunction(PVOID DeviceExtension,
                        PSCSI_REQUEST_BLOCK  Srb)
{
    WNBD_LOG_LOUD(": Enter");

    ASSERT(Srb);
    ASSERT(DeviceExtension);

    StorPortCompleteRequest(DeviceExtension,
                            Srb->PathId,
                            Srb->TargetId,
                            SP_UNTAGGED,
                            SRB_STATUS_TIMEOUT);

    WNBD_LOG_LOUD(": Exit");

    return SRB_STATUS_SUCCESS;
}

_Use_decl_annotations_
UCHAR
WnbdExecuteScsiFunction(PVOID DeviceExtension,
                        PSCSI_REQUEST_BLOCK Srb,
                        PBOOLEAN Complete)
{
    WNBD_LOG_LOUD(": Enter");

    ASSERT(DeviceExtension);
    ASSERT(Srb);
    ASSERT(Complete);

    NTSTATUS Status = STATUS_SUCCESS;
    UCHAR SrbStatus = SRB_STATUS_NO_DEVICE;
    PWNBD_SCSI_DEVICE Device;
    PWNBD_LU_EXTENSION LuExtension;
    PWNBD_EXTENSION DevExtension = (PWNBD_EXTENSION)DeviceExtension;
    KIRQL Irql;
    KSPIN_LOCK DevLock = DevExtension->DeviceListLock;
    KeAcquireSpinLock(&DevLock, &Irql);
    *Complete = TRUE;

    if (SrbGetCdb(Srb)) {
        BYTE CdbValue = SrbGetCdb(Srb)->AsByte[0];

        WNBD_LOG_INFO(": Received %#02x command. SRB = 0x%p. CDB = 0x%x. PathId: %d TargetId: %d LUN: %d",
            CdbValue, Srb, CdbValue, Srb->PathId, Srb->TargetId, Srb->Lun);
    }
    LuExtension = (PWNBD_LU_EXTENSION)
        StorPortGetLogicalUnit(DeviceExtension, Srb->PathId, Srb->TargetId, Srb->Lun );

    if(!LuExtension) {
        WNBD_LOG_ERROR(": Unable to get LUN extension for device PathId: %d TargetId: %d LUN: %d",
                       Srb->PathId, Srb->TargetId, Srb->Lun);
        goto Exit;
    }

    Device = WnbdFindDevice(LuExtension, DeviceExtension,
                            Srb->PathId, Srb->TargetId, Srb->Lun);
    if (NULL == Device) {
        WNBD_LOG_INFO("Could not find device PathId: %d TargetId: %d LUN: %d",
                      Srb->PathId, Srb->TargetId, Srb->Lun);
        goto Exit;
    }

    if (NULL == Device->ScsiDeviceExtension) {
        WNBD_LOG_ERROR("%p has no ScsiDeviceExtension. PathId = %d. TargetId = %d. LUN = %d",
                       Device, Srb->PathId, Srb->TargetId, Srb->Lun);
        goto Exit;
    }
    if (Device->Missing) {
        WNBD_LOG_WARN("%p is marked for deletion. PathId = %d. TargetId = %d. LUN = %d",
                      Device, Srb->PathId, Srb->TargetId, Srb->Lun);
        goto Exit;
    }

    InterlockedIncrement64(&WNBD_DEV_INFO(Device)->Stats.OutstandingIOCount);
    Status = WnbdHandleSrbOperation(DeviceExtension, Device->ScsiDeviceExtension, Srb);

    if(STATUS_PENDING == Status) {
        *Complete = FALSE;
        SrbStatus = SRB_STATUS_PENDING;
    } else {
        InterlockedDecrement64(&WNBD_DEV_INFO(Device)->Stats.OutstandingIOCount);
        SrbStatus = Srb->SrbStatus;
    }

Exit:
    KeReleaseSpinLock(&DevLock, Irql);
    WNBD_LOG_LOUD(": Exit");

    return SrbStatus;
}

_Use_decl_annotations_
UCHAR
WnbdPNPFunction(PSCSI_REQUEST_BLOCK Srb)
{
    WNBD_LOG_LOUD(": Enter");

    ASSERT(Srb);
    PSCSI_PNP_REQUEST_BLOCK PNP = (PSCSI_PNP_REQUEST_BLOCK) Srb;
    UCHAR SrbStatus = SRB_STATUS_INVALID_REQUEST;

    switch (PNP->PnPAction)
    {
    case StorQueryCapabilities:
        if (!(PNP->SrbPnPFlags & SRB_PNP_FLAGS_ADAPTER_REQUEST)) {
            PVOID DataBuffer = SrbGetDataBuffer(Srb);
            ASSERT(DataBuffer);

            PSTOR_DEVICE_CAPABILITIES_EX DeviceCapabilitiesEx = DataBuffer;
            // TODO: check why zero-ing the entire structure leads to a crash
            // on WS 2016.
            RtlZeroMemory(DeviceCapabilitiesEx, sizeof(PSTOR_DEVICE_CAPABILITIES_EX));
            DeviceCapabilitiesEx->DefaultWriteCacheEnabled = 1;
            DeviceCapabilitiesEx->SilentInstall = 1;
            DeviceCapabilitiesEx->SurpriseRemovalOK = 1;
            DeviceCapabilitiesEx->Removable = 1;

            SrbStatus = SRB_STATUS_SUCCESS;
        }
        break;

    default:
        WNBD_LOG_WARN("Untreated SCSI Request PNP Flag: %x", PNP->SrbPnPFlags);
        break;
    }

    WNBD_LOG_LOUD(": Exit with SrbStatus: %s",
                  WnbdToStringSrbStatus(SrbStatus));

    return SrbStatus;
}
