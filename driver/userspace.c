/*
 * Copyright (c) 2019 SUSE LLC
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#include <ntifs.h>

#include <berkeley.h>
#include <ksocket.h>
#include "common.h"
#include "debug.h"
#include "driver_extension.h"
#include "rbd_protocol.h"
#include "scsi_function.h"
#include "userspace.h"
#include "wvbd_dispatch.h"
#include "wvbd_ioctl.h"
#include "util.h"

#define CHECK_I_LOCATION(Io, Type) (Io->Parameters.DeviceIoControl.InputBufferLength < sizeof(Type))
#define CHECK_O_LOCATION(Io, Type) (Io->Parameters.DeviceIoControl.OutputBufferLength < sizeof(Type))
#define CHECK_O_LOCATION_SZ(Io, Size) (Io->Parameters.DeviceIoControl.OutputBufferLength < Size)
#define Malloc(S) ExAllocatePoolWithTag(NonPagedPoolNx, (S), 'DBNu')

extern UNICODE_STRING GlobalRegistryPath;

extern RTL_BITMAP ScsiBitMapHeader = { 0 };
ULONG AssignedScsiIds[((SCSI_MAXIMUM_TARGETS_PER_BUS / 8) / sizeof(ULONG)) * MAX_NUMBER_OF_SCSI_TARGETS];
static ULONG LunId = 0;
VOID WnbdInitScsiIds()
{
    RtlZeroMemory(AssignedScsiIds, sizeof(AssignedScsiIds));
    RtlInitializeBitMap(&ScsiBitMapHeader, AssignedScsiIds, SCSI_MAXIMUM_TARGETS_PER_BUS * MAX_NUMBER_OF_SCSI_TARGETS);
}

_Use_decl_annotations_
BOOLEAN
WnbdFindConnection(PGLOBAL_INFORMATION GInfo,
                   PCHAR InstanceName,
                   PUSER_ENTRY* Entry)
{
    WNBD_LOG_LOUD(": Enter");
    ASSERT(GInfo);
    ASSERT(InstanceName);

    BOOLEAN Found = FALSE;
    PUSER_ENTRY SearchEntry;

    SearchEntry = (PUSER_ENTRY)GInfo->ConnectionList.Flink;

    while (SearchEntry != (PUSER_ENTRY)&GInfo->ConnectionList.Flink) {
        if (!strcmp((CONST CHAR*)&SearchEntry->UserInformation.InstanceName, InstanceName)) {
            if (Entry) {
                *Entry = SearchEntry;

            }
            Found = TRUE;
            break;
        }
        SearchEntry = (PUSER_ENTRY)SearchEntry->ListEntry.Flink;
    }

    WNBD_LOG_LOUD(": Exit");
    return Found;
}

PVOID WnbdCreateScsiDevice(_In_ PVOID Extension,
                           _In_ ULONG PathId,
                           _In_ ULONG TargetId,
                           _In_ ULONG Lun,
                           _In_ PVOID ScsiDeviceExtension,
                           _In_ PINQUIRYDATA InquiryData)
{
    PWNBD_EXTENSION	Ext = (PWNBD_EXTENSION)Extension;
    PWNBD_SCSI_DEVICE Dev = NULL;
    PWNBD_LU_EXTENSION LuExt;

    LuExt = (PWNBD_LU_EXTENSION) StorPortGetLogicalUnit(Ext,
                                   (UCHAR) PathId,
                                   (UCHAR) TargetId,
                                   (UCHAR) Lun);

    if(LuExt) {
        WNBD_LOG_ERROR(": LU extension %p already found for %d:%d:%d", LuExt, PathId, TargetId, Lun);
        return NULL;
    }

    Dev = (PWNBD_SCSI_DEVICE) ExAllocatePoolWithTag(NonPagedPoolNx,sizeof(WNBD_SCSI_DEVICE),'DBNs');

    if(!Dev) {
        WNBD_LOG_ERROR(": Allocation failure");
        return NULL;
    }

    WNBD_LOG_INFO(": Device %p with SCSI_INFO %p and LU extension has "
        "been created for %p at %d:%d:%d",
        Dev, ScsiDeviceExtension, LuExt, PathId, TargetId, Lun);

    RtlZeroMemory(Dev,sizeof(WNBD_SCSI_DEVICE));
    Dev->ScsiDeviceExtension = ScsiDeviceExtension;
    Dev->PathId = PathId;
    Dev->TargetId = TargetId;
    Dev->Lun = Lun;
    Dev->PInquiryData = InquiryData;
    Dev->Missing = FALSE;
    Dev->DriverExtension = (PVOID) Ext;

    WNBD_LOG_LOUD(": Exit");

    return Dev;
}


VOID
WnbdSetInquiryData(_Inout_ PINQUIRYDATA InquiryData)
{
    WNBD_LOG_LOUD(": Enter");
    ASSERT(InquiryData);

    RtlZeroMemory(InquiryData, sizeof(INQUIRYDATA));
    InquiryData->DeviceType = DIRECT_ACCESS_DEVICE;
    InquiryData->DeviceTypeQualifier = DEVICE_QUALIFIER_ACTIVE;
    InquiryData->DeviceTypeModifier = 0;
    InquiryData->RemovableMedia = 0;
    // TODO: consider bumping to SPC-4 or SPC-5.
    InquiryData->Versions = 5;
    InquiryData->ResponseDataFormat = 2;
    InquiryData->Wide32Bit = TRUE;
    InquiryData->Synchronous = FALSE;
    InquiryData->CommandQueue = 1;
    InquiryData->AdditionalLength = INQUIRYDATABUFFERSIZE -
        RTL_SIZEOF_THROUGH_FIELD(INQUIRYDATA, AdditionalLength);
    InquiryData->LinkedCommands = FALSE;
    RtlCopyMemory((PUCHAR)&InquiryData->VendorId[0], WNBD_INQUIRY_VENDOR_ID,
        strlen(WNBD_INQUIRY_VENDOR_ID));
    RtlCopyMemory((PUCHAR)&InquiryData->ProductId[0], WNBD_INQUIRY_PRODUCT_ID,
        strlen(WNBD_INQUIRY_PRODUCT_ID));
    RtlCopyMemory((PUCHAR)&InquiryData->ProductRevisionLevel[0], WNBD_INQUIRY_PRODUCT_REVISION,
        strlen(WNBD_INQUIRY_PRODUCT_REVISION));
    RtlCopyMemory((PUCHAR)&InquiryData->VendorSpecific[0], WNBD_INQUIRY_VENDOR_SPECIFIC,
        strlen(WNBD_INQUIRY_VENDOR_SPECIFIC));

    WNBD_LOG_LOUD(": Exit");
}

#define MallocT(S) ExAllocatePoolWithTag(NonPagedPoolNx, S, 'pDBR')

NTSTATUS
WnbdInitializeScsiInfo(_In_ PSCSI_DEVICE_INFORMATION ScsiInfo)
{
    WNBD_LOG_LOUD(": Enter");
    ASSERT(ScsiInfo);
    HANDLE request_thread_handle = NULL, reply_thread_handle = NULL;
    NTSTATUS Status = STATUS_SUCCESS;

    InitializeListHead(&ScsiInfo->RequestListHead);
    KeInitializeSpinLock(&ScsiInfo->RequestListLock);
    InitializeListHead(&ScsiInfo->ReplyListHead);
    KeInitializeSpinLock(&ScsiInfo->ReplyListLock);
    KeInitializeSemaphore(&ScsiInfo->DeviceEvent, 0, 1 << 30);
    KeInitializeEvent(&ScsiInfo->TerminateEvent, NotificationEvent, FALSE);

    Status = ExInitializeResourceLite(&ScsiInfo->SocketLock);
    if (!NT_SUCCESS(Status)) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    ScsiInfo->HardTerminateDevice = FALSE;
    ScsiInfo->SoftTerminateDevice = FALSE;
    ScsiInfo->ReadPreallocatedBuffer = MallocT(((UINT)WNBD_PREALLOC_BUFF_SZ));
    if (!ScsiInfo->ReadPreallocatedBuffer) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto SoftTerminate;
    }
    ScsiInfo->ReadPreallocatedBufferLength = WNBD_PREALLOC_BUFF_SZ;
    ScsiInfo->WritePreallocatedBuffer = MallocT(((UINT)WNBD_PREALLOC_BUFF_SZ));
    if (!ScsiInfo->WritePreallocatedBuffer) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto SoftTerminate;
    }
    ScsiInfo->WritePreallocatedBufferLength = WNBD_PREALLOC_BUFF_SZ;

    Status = PsCreateSystemThread(&request_thread_handle, (ACCESS_MASK)0L, NULL,
                                  NULL, NULL, WnbdDeviceRequestThread, ScsiInfo);
    if (!NT_SUCCESS(Status)) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto SoftTerminate;
    }

    Status = ObReferenceObjectByHandle(request_thread_handle, THREAD_ALL_ACCESS, NULL, KernelMode,
        &ScsiInfo->DeviceRequestThread, NULL);

    if (!NT_SUCCESS(Status)) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto SoftTerminate;
    }

    Status = PsCreateSystemThread(&reply_thread_handle, (ACCESS_MASK)0L, NULL,
                                  NULL, NULL, WnbdDeviceReplyThread, ScsiInfo);
    if (!NT_SUCCESS(Status)) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto SoftTerminate;
    }

    Status = ObReferenceObjectByHandle(reply_thread_handle, THREAD_ALL_ACCESS, NULL, KernelMode,
        &ScsiInfo->DeviceReplyThread, NULL);

    RtlZeroMemory(&ScsiInfo->Stats, sizeof(WNBD_STATS));

    if (!NT_SUCCESS(Status)) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto SoftTerminate;
    }

    return Status;

SoftTerminate:
    ExDeleteResourceLite(&ScsiInfo->SocketLock);
    if (ScsiInfo->ReadPreallocatedBuffer) {
        ExFreePool(ScsiInfo->ReadPreallocatedBuffer);
    }
    if (ScsiInfo->WritePreallocatedBuffer) {
        ExFreePool(ScsiInfo->WritePreallocatedBuffer);
    }
    if (request_thread_handle)
        ZwClose(request_thread_handle);
    if (reply_thread_handle)
        ZwClose(reply_thread_handle);
    ScsiInfo->SoftTerminateDevice = TRUE;
    KeReleaseSemaphore(&ScsiInfo->DeviceEvent, 0, 1, FALSE);

Exit:
    WNBD_LOG_LOUD(": Exit");
    return Status;
}

NTSTATUS
WnbdInitializeScsiInfoWvbd(_In_ PSCSI_DEVICE_INFORMATION ScsiInfo)
{
    WNBD_LOG_LOUD(": Enter");
    ASSERT(ScsiInfo);
    NTSTATUS Status = STATUS_SUCCESS;

    InitializeListHead(&ScsiInfo->RequestListHead);
    KeInitializeSpinLock(&ScsiInfo->RequestListLock);
    InitializeListHead(&ScsiInfo->ReplyListHead);
    KeInitializeSpinLock(&ScsiInfo->ReplyListLock);
    KeInitializeSemaphore(&ScsiInfo->DeviceEvent, 0, 1 << 30);
    KeInitializeEvent(&ScsiInfo->TerminateEvent, NotificationEvent, FALSE);

    // TODO: check if this is still needed.
    Status = ExInitializeResourceLite(&ScsiInfo->SocketLock);
    if (!NT_SUCCESS(Status)) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    ScsiInfo->HardTerminateDevice = FALSE;
    ScsiInfo->SoftTerminateDevice = FALSE;

    RtlZeroMemory(&ScsiInfo->Stats, sizeof(WNBD_STATS));

Exit:
    WNBD_LOG_LOUD(": Exit");
    return Status;
}

VOID
WnbdSetNullUserInput(PCONNECTION_INFO Info)
{
    Info->InstanceName[MAX_NAME_LENGTH - 1] = '\0';
    Info->Hostname[MAX_NAME_LENGTH - 1] = '\0';
    Info->PortName[MAX_NAME_LENGTH - 1] = '\0';
    Info->ExportName[MAX_NAME_LENGTH - 1] = '\0';
    Info->SerialNumber[MAX_NAME_LENGTH - 1] = '\0';
}

_Use_decl_annotations_
NTSTATUS
WnbdCreateConnection(PGLOBAL_INFORMATION GInfo,
                     PCONNECTION_INFO Info,
                     PWVBD_DISK_HANDLE DiskHandle)
{
    WNBD_LOG_LOUD(": Enter");
    ASSERT(GInfo);
    ASSERT(Info);

    NTSTATUS Status = STATUS_UNSUCCESSFUL;
    BOOLEAN Added = FALSE;
    INT Sock = -1;

    PUSER_ENTRY NewEntry = (PUSER_ENTRY)
        ExAllocatePoolWithTag(NonPagedPoolNx, sizeof(USER_ENTRY), 'DBNu');
    if (!NewEntry) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    Status = KsInitialize();
    if (!NT_SUCCESS(Status)) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    WnbdSetNullUserInput(Info);

    if(WnbdFindConnection(GInfo, Info->InstanceName, NULL)) {
        Status = STATUS_OBJECT_NAME_COLLISION;
        goto Exit;
    }

    RtlZeroMemory(NewEntry,sizeof(USER_ENTRY));
    RtlCopyMemory(&NewEntry->UserInformation, Info, sizeof(CONNECTION_INFO));
    InsertTailList(&GInfo->ConnectionList, &NewEntry->ListEntry);
    Added = TRUE;

    // TODO FOR SN MAYBE ?? status = ExUuidCreate(&tmpGuid);

    PINQUIRYDATA InquiryData = (PINQUIRYDATA) Malloc(sizeof(INQUIRYDATA));
    if (NULL == InquiryData) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    WnbdSetInquiryData(InquiryData);

    Status = STATUS_SUCCESS;
    WNBD_LOG_LOUD("Received disk size: %llu", Info->DiskSize);
    NewEntry->DiskSize = Info->DiskSize;
    NewEntry->BlockSize = LOGICAL_BLOCK_SIZE;
    if (Info->BlockSize) {
        WNBD_LOG_LOUD("Received block size: %u", Info->BlockSize);
        NewEntry->BlockSize = Info->BlockSize;
    }

    if (!Info->UseWvbdAPI) {
        Sock = NbdOpenAndConnect(Info->Hostname, Info->PortName);
        if (-1 == Sock) {
            Status = STATUS_CONNECTION_REFUSED;
            goto ExitInquiryData;
        }
    }

    ULONG bitNumber = RtlFindClearBitsAndSet(&ScsiBitMapHeader, 1, 0);

    if (0xFFFFFFFF == bitNumber) {
        Status = STATUS_INVALID_FIELD_IN_PARAMETER_LIST;
        goto ExitInquiryData;
    }

    UINT16 RbdFlags = 0;
    if (!Info->UseWvbdAPI && Info->MustNegotiate) {
        WNBD_LOG_INFO("Trying to negotiate handshake with RBD Server");
        Info->DiskSize = 0;
        Status = RbdNegotiate(&Sock, &Info->DiskSize, &RbdFlags, Info->ExportName, 1, 1);
        if (!NT_SUCCESS(Status)) {
            goto ExitInquiryData;
        }
        WNBD_LOG_INFO("Negotiated disk size: %llu", Info->DiskSize);
        NewEntry->DiskSize = Info->DiskSize;
    }

    NewEntry->NbdFlags = Info->NbdFlags | RbdFlags;
    ULONG TargetId = bitNumber % SCSI_MAXIMUM_TARGETS_PER_BUS;
    ULONG BusId = bitNumber / MAX_NUMBER_OF_SCSI_TARGETS;

    WNBD_LOG_INFO("Retrieved NBD flags: %d. Read-only: %d, TRIM enabled: %d, "
                  "FLUSH enabled: %d, FUA enabled: %d.",
                   NewEntry->NbdFlags,
                   CHECK_NBD_READONLY(NewEntry->NbdFlags),
                   CHECK_NBD_SEND_TRIM(NewEntry->NbdFlags),
                   CHECK_NBD_SEND_FLUSH(NewEntry->NbdFlags),
                   CHECK_NBD_SEND_FUA(NewEntry->NbdFlags));

    PSCSI_DEVICE_INFORMATION ScsiInfo = (PSCSI_DEVICE_INFORMATION) Malloc(sizeof(SCSI_DEVICE_INFORMATION));
    if(!ScsiInfo) {
        Status = STATUS_INSUFFICIENT_RESOURCES;
        goto ExitInquiryData;
    }

    RtlZeroMemory(ScsiInfo, sizeof(SCSI_DEVICE_INFORMATION));
    ScsiInfo->Device = WnbdCreateScsiDevice(GInfo->Handle,
                                            BusId,
                                            TargetId,
                                            LunId,
                                            ScsiInfo,
                                            InquiryData);

    if (!ScsiInfo->Device) {
        Status = STATUS_DEVICE_ALREADY_ATTACHED;
        goto ExitScsiInfo;
    }

    ScsiInfo->GlobalInformation = GInfo;
    ScsiInfo->InquiryData = InquiryData;
    ScsiInfo->Socket = Sock;

    if (Info->UseWvbdAPI)
        Status = WnbdInitializeScsiInfoWvbd(ScsiInfo);
    else
        Status = WnbdInitializeScsiInfo(ScsiInfo);

    if (!NT_SUCCESS(Status)) {
        goto ExitScsiInfo;
    }

    // TODO: why do we store the same info twice?
    ScsiInfo->UserEntry = NewEntry;
    ScsiInfo->TargetIndex = TargetId;
    ScsiInfo->BusIndex = BusId;
    ScsiInfo->LunIndex = LunId;
    *DiskHandle = WVBD_DISK_HANDLE_FROM_ADDR(TargetId, BusId, LunId);

    NewEntry->ScsiInformation = ScsiInfo;
    NewEntry->BusIndex = BusId;
    NewEntry->TargetIndex = TargetId;
    NewEntry->LunIndex = LunId;

    PWNBD_EXTENSION	Ext = (PWNBD_EXTENSION)GInfo->Handle;
    KeEnterCriticalRegion();
    ExAcquireResourceSharedLite(&Ext->DeviceResourceLock, TRUE);

    InsertTailList(&Ext->DeviceList, &ScsiInfo->Device->ListEntry);

    ExReleaseResourceLite(&Ext->DeviceResourceLock);
    KeLeaveCriticalRegion();

    InterlockedIncrement(&GInfo->ConnectionCount);
    StorPortNotification(BusChangeDetected, GInfo->Handle, 0);

    NewEntry->Connected = TRUE;
    Status = STATUS_SUCCESS;

    WNBD_LOG_LOUD(": Exit");

    return Status;

ExitScsiInfo:
    if (ScsiInfo) {
        if (ScsiInfo->Device) {
            ExFreePool(ScsiInfo->Device);
        }
        ExFreePool(ScsiInfo);
    }
ExitInquiryData:
    if (InquiryData) {
        ExFreePool(InquiryData);
    }
Exit:
    if (-1 != Sock) {
        WNBD_LOG_ERROR("Closing socket FD: %d", Sock);
        Close(Sock);
        Sock = -1;
    }
    if (Added) {
        WnbdDeleteConnectionEntry(NewEntry);
    }
    if (NewEntry) {
        ExFreePool(NewEntry);
    }

    WNBD_LOG_LOUD(": Exit");
    return Status;
}

_Use_decl_annotations_
NTSTATUS
WnbdDeleteConnectionEntry(PUSER_ENTRY Entry)
{
    WNBD_LOG_LOUD(": Enter");
    ASSERT(Entry);

    RemoveEntryList(&Entry->ListEntry);

    WNBD_LOG_LOUD(": Exit");
    return STATUS_SUCCESS;
}

BOOLEAN
WnbdSetDeviceMissing(_In_ PVOID Handle,
                     _In_ BOOLEAN Force)
{
    WNBD_LOG_LOUD(": Enter");
    PWNBD_SCSI_DEVICE Device = (PWNBD_SCSI_DEVICE)Handle;
    
    if (Device == NULL) {
        return TRUE;
    }

    if(Device->OutstandingIoCount && !Force) {
        return FALSE;
    }
    
    ASSERT(!Device->OutstandingIoCount);
    WNBD_LOG_INFO("Disconnecting with, OutstandingIoCount: %d", Device->OutstandingIoCount);

    Device->Missing = TRUE;

    WNBD_LOG_LOUD(": Exit");

    return TRUE;
}


VOID
WnbdDrainQueueOnClose(_In_ PSCSI_DEVICE_INFORMATION DeviceInformation)
{
    PLIST_ENTRY ItemLink, ItemNext;
    KIRQL Irql = { 0 };
    PSRB_QUEUE_ELEMENT Element = NULL;

    KeAcquireSpinLock(&DeviceInformation->RequestListLock, &Irql);
    if (IsListEmpty(&DeviceInformation->RequestListHead))
        goto Reply;

    LIST_FORALL_SAFE(&DeviceInformation->RequestListHead, ItemLink, ItemNext) {
        Element = CONTAINING_RECORD(ItemLink, SRB_QUEUE_ELEMENT, Link);
        if (Element) {
            RemoveEntryList(&Element->Link);
            Element->Srb->DataTransferLength = 0;
            Element->Srb->SrbStatus = SRB_STATUS_INTERNAL_ERROR;
            StorPortNotification(RequestComplete, Element->DeviceExtension,
                Element->Srb);
            ExFreePool(Element);
        }
        Element = NULL;
    }
Reply:
    KeReleaseSpinLock(&DeviceInformation->RequestListLock, Irql);
    KeAcquireSpinLock(&DeviceInformation->ReplyListLock, &Irql);
    if (IsListEmpty(&DeviceInformation->ReplyListHead))
        goto Exit;

    LIST_FORALL_SAFE(&DeviceInformation->ReplyListHead, ItemLink, ItemNext) {
        Element = CONTAINING_RECORD(ItemLink, SRB_QUEUE_ELEMENT, Link);
        if (Element) {
            RemoveEntryList(&Element->Link);
            if (!Element->Aborted) {
                Element->Srb->DataTransferLength = 0;
                Element->Srb->SrbStatus = SRB_STATUS_INTERNAL_ERROR;
                StorPortNotification(RequestComplete, Element->DeviceExtension,
                    Element->Srb);
            }
            ExFreePool(Element);
            InterlockedDecrement64(&DeviceInformation->Stats.PendingSubmittedIORequests);
        }
        Element = NULL;
    }
Exit:
    KeReleaseSpinLock(&DeviceInformation->ReplyListLock, Irql);
}

_Use_decl_annotations_
NTSTATUS
WnbdDeleteConnection(PGLOBAL_INFORMATION GInfo,
                     PCHAR InstanceName)
{
    WNBD_LOG_LOUD(": Enter");
    ASSERT(GInfo);
    ASSERT(InstanceName);
    PUSER_ENTRY EntryMarked = NULL;
    ULONG TargetIndex = 0;
    ULONG BusIndex = 0;
    if(!WnbdFindConnection(GInfo, InstanceName, &EntryMarked)) {
        WNBD_LOG_ERROR("Could not find connection to delete");
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    if (NULL == EntryMarked) {
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }
    PSCSI_DEVICE_INFORMATION ScsiInfo = EntryMarked->ScsiInformation;
    NTSTATUS Status = STATUS_UNSUCCESSFUL;

    if(ScsiInfo) {
        TargetIndex = ScsiInfo->TargetIndex;
        BusIndex = ScsiInfo->BusIndex;
        ScsiInfo->SoftTerminateDevice = TRUE;
        // TODO: implement proper soft termination.
        ScsiInfo->HardTerminateDevice = TRUE;
        KeSetEvent(&ScsiInfo->TerminateEvent, IO_NO_INCREMENT, FALSE);
        KeReleaseSemaphore(&ScsiInfo->DeviceEvent, 0, 1, FALSE);
        LARGE_INTEGER Timeout;
        // TODO: consider making this configurable, currently 120s.
        Timeout.QuadPart = (-120 * 1000 * 10000);
        CloseConnection(ScsiInfo);

        if (!ScsiInfo->UserEntry->UserInformation.UseWvbdAPI) {
            KeWaitForSingleObject(ScsiInfo->DeviceRequestThread, Executive, KernelMode, FALSE, NULL);
            KeWaitForSingleObject(ScsiInfo->DeviceReplyThread, Executive, KernelMode, FALSE, &Timeout);
            ObDereferenceObject(ScsiInfo->DeviceRequestThread);
            ObDereferenceObject(ScsiInfo->DeviceReplyThread);
        }
        WnbdDrainQueueOnClose(ScsiInfo);
        DisconnectConnection(ScsiInfo);

        if(!WnbdSetDeviceMissing(ScsiInfo->Device,TRUE)) {
            WNBD_LOG_WARN("Could not delete media because it is still in use.");
            return STATUS_UNABLE_TO_UNLOAD_MEDIA;
        }
        StorPortNotification(BusChangeDetected, GInfo->Handle, 0);
    } else {
        WNBD_LOG_ERROR("Could not find device needed for deletion");
        return STATUS_OBJECT_NAME_NOT_FOUND;
    }

    Status = WnbdDeleteConnectionEntry(EntryMarked);

    RtlClearBits(&ScsiBitMapHeader, TargetIndex + (BusIndex * SCSI_MAXIMUM_TARGETS_PER_BUS), 1);

    InterlockedDecrement(&GInfo->ConnectionCount);
    WNBD_LOG_LOUD(": Exit");

    return Status;
}

// TODO: deprecate this in favor of WnbdEnumerateActiveConnectionsEx
_Use_decl_annotations_
NTSTATUS
WnbdEnumerateActiveConnections(PGLOBAL_INFORMATION GInfo,
                               PIRP Irp)
{
    WNBD_LOG_LOUD(": Enter");
    ASSERT(GInfo);
    ASSERT(Irp);

    PUSER_ENTRY SearchEntry;
    PIO_STACK_LOCATION IoLocation = IoGetCurrentIrpStackLocation(Irp);
    PDISK_INFO_LIST OutList = (PDISK_INFO_LIST) Irp->AssociatedIrp.SystemBuffer;
    PDISK_INFO OutEntry = &OutList->ActiveEntry[0];
    ULONG Remaining;
    NTSTATUS status = STATUS_BUFFER_OVERFLOW;

    OutList->ActiveListCount = 0;

    Remaining = (IoLocation->Parameters.DeviceIoControl.OutputBufferLength -
        sizeof(ULONG))/sizeof(DISK_INFO);

    SearchEntry = (PUSER_ENTRY)GInfo->ConnectionList.Flink;

    while((SearchEntry != (PUSER_ENTRY) &GInfo->ConnectionList.Flink) && Remaining) {
        OutEntry = &OutList->ActiveEntry[OutList->ActiveListCount];
        RtlZeroMemory(OutEntry, sizeof(DISK_INFO));
        RtlCopyMemory(OutEntry, &SearchEntry->UserInformation, sizeof(CONNECTION_INFO));

        OutEntry->BusNumber = (USHORT)SearchEntry->BusIndex;
        OutEntry->TargetId = (USHORT)SearchEntry->TargetIndex;
        OutEntry->Lun = (USHORT)SearchEntry->LunIndex;
        OutEntry->Connected = SearchEntry->Connected;
        OutEntry->DiskSize = SearchEntry->UserInformation.DiskSize;

        WNBD_LOG_INFO(": %d:%d:%d Connected: %d",
            SearchEntry->BusIndex, SearchEntry->TargetIndex, SearchEntry->LunIndex,
            OutEntry->Connected);

        OutList->ActiveListCount++;
        Remaining--;

        SearchEntry = (PUSER_ENTRY)SearchEntry->ListEntry.Flink;
    }

    if(SearchEntry == (PUSER_ENTRY) &GInfo->ConnectionList.Flink) {
        status = STATUS_SUCCESS;
    }

    Irp->IoStatus.Information = (OutList->ActiveListCount * sizeof(DISK_INFO)) + sizeof(ULONG);
    WNBD_LOG_LOUD(": Exit: %d", status);

    return status;
}

_Use_decl_annotations_
NTSTATUS
WnbdEnumerateActiveConnectionsEx(PGLOBAL_INFORMATION GInfo, PIRP Irp)
{
    WNBD_LOG_LOUD(": Enter");
    ASSERT(GInfo);
    ASSERT(Irp);

    PUSER_ENTRY CurrentEntry = (PUSER_ENTRY)GInfo->ConnectionList.Flink;
    PIO_STACK_LOCATION IoLocation = IoGetCurrentIrpStackLocation(Irp);
    PWVBD_CONNECTION_LIST OutList = (PWVBD_CONNECTION_LIST) Irp->AssociatedIrp.SystemBuffer;
    PWVBD_CONNECTION_INFO OutEntry = &OutList->Connections[0];

    NTSTATUS Status = STATUS_BUFFER_OVERFLOW;
    ULONG Remaining = (
        IoLocation->Parameters.DeviceIoControl.OutputBufferLength -
            RTL_SIZEOF_THROUGH_FIELD(WVBD_CONNECTION_LIST, Count)
        ) / sizeof(WVBD_CONNECTION_INFO);
    OutList->Count = 0;
    OutList->ElementSize = sizeof(WVBD_CONNECTION_INFO);

    while((CurrentEntry != (PUSER_ENTRY) &GInfo->ConnectionList.Flink) && Remaining) {
        OutEntry = &OutList->Connections[OutList->Count];
        RtlZeroMemory(OutEntry, sizeof(WVBD_CONNECTION_INFO));

        // TODO: we'll avoid converting those structures once we use the new version internally.
        // We're also skipping NBD params for now.
        RtlCopyMemory(OutEntry->Properties.InstanceName, &CurrentEntry->UserInformation.InstanceName, MAX_NAME_LENGTH);
        RtlCopyMemory(OutEntry->Properties.SerialNumber, &CurrentEntry->UserInformation.SerialNumber, MAX_NAME_LENGTH);

        OutEntry->Properties.Pid = CurrentEntry->UserInformation.Pid;
        OutEntry->Properties.BlockSize = CurrentEntry->UserInformation.BlockSize;
        OutEntry->Properties.BlockCount = CurrentEntry->UserInformation.DiskSize / CurrentEntry->UserInformation.BlockSize;
        OutEntry->Properties.Flags.UseNbd = !CurrentEntry->UserInformation.UseWvbdAPI;

        RtlCopyMemory(OutEntry, &CurrentEntry->UserInformation, sizeof(CONNECTION_INFO));

        OutEntry->BusNumber = (USHORT)CurrentEntry->BusIndex;
        OutEntry->TargetId = (USHORT)CurrentEntry->TargetIndex;
        OutEntry->Lun = (USHORT)CurrentEntry->LunIndex;

        OutList->Count++;
        Remaining--;

        CurrentEntry = (PUSER_ENTRY)CurrentEntry->ListEntry.Flink;
    }

    if(CurrentEntry == (PUSER_ENTRY) &GInfo->ConnectionList.Flink) {
        Status = STATUS_SUCCESS;
    }

    Irp->IoStatus.Information = (OutList->Count * sizeof(WVBD_CONNECTION_INFO)) +
        RTL_SIZEOF_THROUGH_FIELD(WVBD_CONNECTION_LIST, Count);
    WNBD_LOG_LOUD(": Exit: %d", Status);

    return Status;
}

_Use_decl_annotations_
NTSTATUS
WnbdParseUserIOCTL(PVOID GlobalHandle,
                   PIRP Irp)
{
    WNBD_LOG_LOUD(": Enter");
    ASSERT(Irp);
    ASSERT(GlobalHandle);
    PIO_STACK_LOCATION IoLocation = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS Status = STATUS_SUCCESS;
    PGLOBAL_INFORMATION	GInfo = (PGLOBAL_INFORMATION) GlobalHandle;

    PWNBD_SCSI_DEVICE Device = NULL;
    PWNBD_EXTENSION Ext = NULL;

    WNBD_LOG_LOUD(": DeviceIoControl = 0x%x.",
                  IoLocation->Parameters.DeviceIoControl.IoControlCode);

    switch (IoLocation->Parameters.DeviceIoControl.IoControlCode) {
    case IOCTL_MINIPORT_PROCESS_SERVICE_IRP:
        {
        PWNBD_COMMAND Code = (PWNBD_COMMAND)Irp->AssociatedIrp.SystemBuffer;
        if (NULL == Code || CHECK_I_LOCATION(IoLocation, WNBD_COMMAND)) {
            WNBD_LOG_ERROR(": IOCTL = 0x%x. Bad input buffer",
                IoLocation->Parameters.DeviceIoControl.IoControlCode);
            return STATUS_INVALID_PARAMETER;
        }

        switch(Code->IoCode) {

        case IOCTL_WNBD_PORT:
        case IOCTL_WVBD_PING:
            {
            WNBD_LOG_LOUD("IOCTL_WVBD_PING");
            Status = STATUS_SUCCESS;
            }
            break;

        case IOCTL_WNBD_MAP:
            {
            WNBD_LOG_LOUD("IOCTL_WNBDVM_MAP");
            PCONNECTION_INFO Info = (PCONNECTION_INFO) Irp->AssociatedIrp.SystemBuffer;
            if(NULL == Info || CHECK_I_LOCATION(IoLocation, CONNECTION_INFO)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Bad input buffer",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            if(!wcslen((PWSTR) &Info->InstanceName)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. InstanceName Error",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            KeEnterCriticalRegion();
            ExAcquireResourceExclusiveLite(&GInfo->ConnectionMutex, TRUE);

            if(WnbdFindConnection(GInfo, Info->InstanceName, NULL)) {
                ExReleaseResourceLite(&GInfo->ConnectionMutex);
                KeLeaveCriticalRegion();
                Status = STATUS_FILES_OPEN;
                WNBD_LOG_ERROR(": IOCTL = 0x%x. EEXIST",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                break;
            }
            WNBD_LOG_LOUD("IOCTL_WNBDVM_MAP CreateConnection");
            WVBD_DISK_HANDLE DiskHandle = 0;
            Status = WnbdCreateConnection(GInfo, Info, &DiskHandle);
            ExReleaseResourceLite(&GInfo->ConnectionMutex);
            KeLeaveCriticalRegion();
            }
            break;

        case IOCTL_WNBD_UNMAP:
            {
            WNBD_LOG_LOUD("IOCTL_WNBDVM_UNMAP");
            PCONNECTION_INFO Info = (PCONNECTION_INFO) Irp->AssociatedIrp.SystemBuffer;

            if(!Info || CHECK_I_LOCATION(IoLocation, CONNECTION_INFO)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Bad input buffer",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            if(!wcslen((PWSTR) &Info->InstanceName)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. InstanceName Error",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            KeEnterCriticalRegion();
            ExAcquireResourceExclusiveLite(&GInfo->ConnectionMutex, TRUE);
            if(!WnbdFindConnection(GInfo, Info->InstanceName, NULL)) {
                ExReleaseResourceLite(&GInfo->ConnectionMutex);
                KeLeaveCriticalRegion();
                Status = STATUS_OBJECT_NAME_NOT_FOUND;
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Connection does not exist",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                break;
            }
            WNBD_LOG_LOUD("IOCTL_WNBDVM_UNMAP DeleteConnection");
            Status = WnbdDeleteConnection(GInfo, Info->InstanceName);
            ExReleaseResourceLite(&GInfo->ConnectionMutex);
            KeLeaveCriticalRegion();
            }
            break;

        case IOCTL_WNBD_LIST:
            {
            WNBD_LOG_LOUD("IOCTL_WNBDVM_LIST");
            KeEnterCriticalRegion();
            ExAcquireResourceExclusiveLite(&GInfo->ConnectionMutex, TRUE);
            if(!Irp->AssociatedIrp.SystemBuffer || CHECK_O_LOCATION(IoLocation, DISK_INFO_LIST)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Bad output buffer",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);

                Irp->IoStatus.Information = (GInfo->ConnectionCount * sizeof(DISK_INFO) ) + sizeof(DISK_INFO_LIST);
                ExReleaseResourceLite(&GInfo->ConnectionMutex);
                KeLeaveCriticalRegion();
                break;
            }
            Status = WnbdEnumerateActiveConnections(GInfo, Irp);
            ExReleaseResourceLite(&GInfo->ConnectionMutex);
            KeLeaveCriticalRegion();
            }
            break;

        case IOCTL_WNBD_DEBUG:
        case IOCTL_WVBD_RELOAD_CONFIG:
        {
            WNBD_LOG_LOUD("IOCTL_WVBD_RELOAD_CONFIG");
            WCHAR* KeyName = L"DebugLogLevel";
            UINT32 U32Val = 0;
            if (WNBDReadRegistryValue(&GlobalRegistryPath, KeyName,
                (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT), &U32Val)) {
                WnbdSetLogLevel(U32Val);
            }
        }
        break;

        case IOCTL_WNBD_STATS:
        {
            // Retrieve per mapping stats. TODO: consider providing global stats.
            WNBD_LOG_LOUD("IOCTL_WNBDVM_STATS");
            PCONNECTION_INFO Info = (PCONNECTION_INFO) Irp->AssociatedIrp.SystemBuffer;

            if(!Info || CHECK_I_LOCATION(IoLocation, CONNECTION_INFO)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Bad input buffer",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            if(!wcslen((PWSTR) &Info->InstanceName)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. InstanceName Error",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            KeEnterCriticalRegion();
            ExAcquireResourceExclusiveLite(&GInfo->ConnectionMutex, TRUE);
            if(!Irp->AssociatedIrp.SystemBuffer || CHECK_O_LOCATION(IoLocation, WNBD_STATS)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Bad output buffer",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);

                Irp->IoStatus.Information = sizeof(WNBD_STATS);
                ExReleaseResourceLite(&GInfo->ConnectionMutex);
                KeLeaveCriticalRegion();
                break;
            }

            PUSER_ENTRY DiskEntry = NULL;
            if(!WnbdFindConnection(GInfo, Info->InstanceName, &DiskEntry)) {
                ExReleaseResourceLite(&GInfo->ConnectionMutex);
                KeLeaveCriticalRegion();
                Status = STATUS_OBJECT_NAME_NOT_FOUND;
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Connection does not exist",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                break;
            }

            PWNBD_STATS OutStatus = (PWNBD_STATS) Irp->AssociatedIrp.SystemBuffer;
            RtlCopyMemory(OutStatus, &DiskEntry->ScsiInformation->Stats, sizeof(WNBD_STATS));

            Irp->IoStatus.Information = sizeof(WNBD_STATS);
            ExReleaseResourceLite(&GInfo->ConnectionMutex);
            KeLeaveCriticalRegion();

            Status = STATUS_SUCCESS;
        }
        break;

        case IOCTL_WVBD_CREATE:
            WNBD_LOG_LOUD("IOCTL_WVBD_CREATE");
            PWVBD_IOCTL_CREATE_COMMAND Command = (PWVBD_IOCTL_CREATE_COMMAND) Irp->AssociatedIrp.SystemBuffer;
            if(!Command ||
                CHECK_I_LOCATION(IoLocation, WVBD_IOCTL_CREATE_COMMAND) ||
                CHECK_O_LOCATION(IoLocation, WVBD_DISK_HANDLE))
            {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Bad input or output buffer",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            WVBD_PROPERTIES Info = Command->Properties;
            Info.InstanceName[MAX_NAME_LENGTH - 1] = '\0';
            Info.SerialNumber[MAX_NAME_LENGTH - 1] = '\0';

            if(!strlen((char*)&Info.InstanceName)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Invalid InstanceName",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }
            KeEnterCriticalRegion();
            ExAcquireResourceExclusiveLite(&GInfo->ConnectionMutex, TRUE);

            // TODO: try to unify the API
            CONNECTION_INFO ConnInfo = {0};
            RtlCopyMemory(ConnInfo.InstanceName, Info.InstanceName, MAX_NAME_LENGTH);
            if (strlen(Info.SerialNumber))
                RtlCopyMemory(ConnInfo.SerialNumber, Info.SerialNumber, MAX_NAME_LENGTH);
            else
                RtlCopyMemory(ConnInfo.SerialNumber, Info.InstanceName, MAX_NAME_LENGTH);
            ConnInfo.BlockSize = (UINT16)Info.BlockSize;
            ConnInfo.DiskSize = Info.BlockSize * Info.BlockCount;
            ConnInfo.Pid = IoGetRequestorProcessId(Irp);
            ConnInfo.UseWvbdAPI = 1;
            // TODO: populate NBD flags

            WNBD_LOG_INFO("Mapping disk. Name: %s, Serial=%s, BC=%llu, BS=%lu, Pid=%d",
                          Info.InstanceName, Info.SerialNumber,
                          Info.BlockCount, Info.BlockSize, ConnInfo.Pid);

            if(WnbdFindConnection(GInfo, ConnInfo.InstanceName, NULL)) {
                ExReleaseResourceLite(&GInfo->ConnectionMutex);
                KeLeaveCriticalRegion();
                Status = STATUS_FILES_OPEN;
                WNBD_LOG_ERROR(": IOCTL = 0x%x. InstanceName already used.",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                break;
            }

            WVBD_DISK_HANDLE DiskHandle = 0;
            Status = WnbdCreateConnection(GInfo, &ConnInfo, &DiskHandle);

            PWVBD_DISK_HANDLE OutHandle = (PWVBD_DISK_HANDLE) Irp->AssociatedIrp.SystemBuffer;
            RtlCopyMemory(OutHandle, &DiskHandle, sizeof(WVBD_DISK_HANDLE));
            Irp->IoStatus.Information = sizeof(WVBD_DISK_HANDLE);

            WNBD_LOG_LOUD("Mapped disk. Name: %s, Handle: %llu", Info.InstanceName, DiskHandle);

            ExReleaseResourceLite(&GInfo->ConnectionMutex);
            KeLeaveCriticalRegion();
            break;

        case IOCTL_WVBD_REMOVE:
            {
            WNBD_LOG_LOUD("IOCTL_WVBD_REMOVE");
            PWVBD_IOCTL_REMOVE_COMMAND RmCmd = (PWVBD_IOCTL_REMOVE_COMMAND) Irp->AssociatedIrp.SystemBuffer;

            if(!RmCmd || CHECK_I_LOCATION(IoLocation, WVBD_IOCTL_REMOVE_COMMAND)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Bad input buffer",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            RmCmd->InstanceName[MAX_NAME_LENGTH - 1] = '\0';
            if(!strlen((PCHAR)RmCmd->InstanceName)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. InstanceName Error",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            KeEnterCriticalRegion();
            ExAcquireResourceExclusiveLite(&GInfo->ConnectionMutex, TRUE);

            // TODO: avoid duplicating WNBD unmap code.
            if(!WnbdFindConnection(GInfo, RmCmd->InstanceName, NULL)) {
                ExReleaseResourceLite(&GInfo->ConnectionMutex);
                KeLeaveCriticalRegion();
                Status = STATUS_OBJECT_NAME_NOT_FOUND;
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Connection does not exist",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                break;
            }
            WNBD_LOG_LOUD("IOCTL_WNBDVM_UNMAP DeleteConnection");
            Status = WnbdDeleteConnection(GInfo, RmCmd->InstanceName);
            ExReleaseResourceLite(&GInfo->ConnectionMutex);
            KeLeaveCriticalRegion();
            }
            break;

        case IOCTL_WVBD_FETCH_REQ:
            // TODO: move out individual Ioctl handling (e.g. adding
            // WvbdIoctlFetchReq), this is becoming unreadable.
            WNBD_LOG_LOUD("IOCTL_WVBD_FETCH_REQ");
            PWVBD_IOCTL_FETCH_REQ_COMMAND ReqCmd =
                (PWVBD_IOCTL_FETCH_REQ_COMMAND) Irp->AssociatedIrp.SystemBuffer;
            // TODO: check if we can alter the input buffer without passing it twice.
            if(!ReqCmd ||
                CHECK_I_LOCATION(IoLocation, PWVBD_IOCTL_FETCH_REQ_COMMAND) ||
                CHECK_O_LOCATION(IoLocation, PWVBD_IOCTL_FETCH_REQ_COMMAND))
            {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Bad input/output buffer",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            // TODO: do we actually need to enter a critical section and
            // use the connection mutex?
            KeEnterCriticalRegion();
            ExAcquireResourceExclusiveLite(&GInfo->ConnectionMutex, TRUE);

            Ext = (PWNBD_EXTENSION)GInfo->Handle;
            Device = WnbdFindDeviceEx(
                Ext,
                WVBD_DISK_HANDLE_PATH(ReqCmd->DiskHandle),
                WVBD_DISK_HANDLE_TARGET(ReqCmd->DiskHandle),
                WVBD_DISK_HANDLE_LUN(ReqCmd->DiskHandle));

            ExReleaseResourceLite(&GInfo->ConnectionMutex);
            KeLeaveCriticalRegion();

            if (!Device) {
                Status = STATUS_INVALID_HANDLE;
                WNBD_LOG_ERROR(
                    ": IOCTL = 0x%x. Could not fetch request, invalid disk handle: %d.",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode,
                    ReqCmd->DiskHandle);
                break;
            }

            Status = WvbdDispatchRequest(Irp, Device, ReqCmd);
            Irp->IoStatus.Information = sizeof(WVBD_IOCTL_FETCH_REQ_COMMAND);
            WNBD_LOG_LOUD("Request dispatch status: %d. Request type: %d Request handle: %llx",
                          Status, ReqCmd->Request.RequestType, ReqCmd->Request.RequestHandle);
            break;

        case IOCTL_WVBD_SEND_RSP:
            WNBD_LOG_LOUD("IOCTL_WVBD_SEND_RSP");
            PWVBD_IOCTL_SEND_RSP_COMMAND RspCmd =
                (PWVBD_IOCTL_SEND_RSP_COMMAND) Irp->AssociatedIrp.SystemBuffer;
            if(!RspCmd || CHECK_I_LOCATION(IoLocation, PWVBD_IOCTL_FETCH_REQ_COMMAND)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Bad input/output buffer",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            KeEnterCriticalRegion();
            ExAcquireResourceExclusiveLite(&GInfo->ConnectionMutex, TRUE);

            Ext = (PWNBD_EXTENSION)GInfo->Handle;
            Device = WnbdFindDeviceEx(
                Ext,
                WVBD_DISK_HANDLE_PATH(RspCmd->DiskHandle),
                WVBD_DISK_HANDLE_TARGET(RspCmd->DiskHandle),
                WVBD_DISK_HANDLE_LUN(RspCmd->DiskHandle));

            ExReleaseResourceLite(&GInfo->ConnectionMutex);
            KeLeaveCriticalRegion();

            if (!Device) {
                Status = STATUS_INVALID_HANDLE;
                WNBD_LOG_ERROR(
                    ": IOCTL = 0x%x. Could not fetch request, invalid disk handle: %d.",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode,
                    RspCmd->DiskHandle);
                break;
            }

            Status = WvbdHandleResponse(Irp, Device, RspCmd);
            WNBD_LOG_LOUD("Reply handling status: %d.", Status);
            break;

        case IOCTL_WVBD_LIST:
            {
            WNBD_LOG_LOUD("IOCTL_WVBD_LIST");
            KeEnterCriticalRegion();
            ExAcquireResourceExclusiveLite(&GInfo->ConnectionMutex, TRUE);
            DWORD RequiredBuffSize = (
                GInfo->ConnectionCount * sizeof(WVBD_CONNECTION_INFO)) + sizeof(WVBD_CONNECTION_LIST);

            if (!Irp->AssociatedIrp.SystemBuffer || CHECK_O_LOCATION_SZ(IoLocation, RequiredBuffSize)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Bad output buffer",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);

                Irp->IoStatus.Information = RequiredBuffSize;
                ExReleaseResourceLite(&GInfo->ConnectionMutex);
                KeLeaveCriticalRegion();
                break;
            }
            Status = WnbdEnumerateActiveConnectionsEx(GInfo, Irp);
            ExReleaseResourceLite(&GInfo->ConnectionMutex);
            KeLeaveCriticalRegion();
            }
            break;

        case IOCTL_WVBD_STATS:
        {
            WNBD_LOG_LOUD("WVBD_STATS");
            PWVBD_IOCTL_STATS_COMMAND StatsCmd =
                (PWVBD_IOCTL_STATS_COMMAND) Irp->AssociatedIrp.SystemBuffer;

            if(!StatsCmd || CHECK_I_LOCATION(IoLocation, WVBD_IOCTL_STATS_COMMAND)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Bad input buffer",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            StatsCmd->InstanceName[MAX_NAME_LENGTH] = '\0';
            if(!strlen((PSTR) &StatsCmd->InstanceName)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. InstanceName Error",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                Status = STATUS_INVALID_PARAMETER;
                break;
            }

            KeEnterCriticalRegion();
            ExAcquireResourceExclusiveLite(&GInfo->ConnectionMutex, TRUE);
            if(!Irp->AssociatedIrp.SystemBuffer || CHECK_O_LOCATION(IoLocation,
                    WVBD_DRV_STATS)) {
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Bad output buffer",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);

                Irp->IoStatus.Information = sizeof(WVBD_DRV_STATS);
                ExReleaseResourceLite(&GInfo->ConnectionMutex);
                KeLeaveCriticalRegion();
                break;
            }

            PUSER_ENTRY DiskEntry = NULL;
            if(!WnbdFindConnection(GInfo, StatsCmd->InstanceName, &DiskEntry)) {
                ExReleaseResourceLite(&GInfo->ConnectionMutex);
                KeLeaveCriticalRegion();
                Status = STATUS_OBJECT_NAME_NOT_FOUND;
                WNBD_LOG_ERROR(": IOCTL = 0x%x. Connection does not exist",
                    IoLocation->Parameters.DeviceIoControl.IoControlCode);
                break;
            }

            PWNBD_STATS OutStatus = (PWNBD_STATS) Irp->AssociatedIrp.SystemBuffer;
            RtlCopyMemory(OutStatus, &DiskEntry->ScsiInformation->Stats,
                          sizeof(WVBD_DRV_STATS));

            Irp->IoStatus.Information = sizeof(WVBD_DRV_STATS);
            ExReleaseResourceLite(&GInfo->ConnectionMutex);
            KeLeaveCriticalRegion();

            Status = STATUS_SUCCESS;
        }
        break;

        default:
            {
            WNBD_LOG_ERROR("ScsiPortDeviceControl: Unsupported IOCTL (%x)",
                IoLocation->Parameters.DeviceIoControl.IoControlCode);
            Status = STATUS_INVALID_DEVICE_REQUEST;
            }
            break;
        }
        }
        break;

    default:
        WNBD_LOG_ERROR("Unsupported IOCTL (%x)",
                      IoLocation->Parameters.DeviceIoControl.IoControlCode);
        Status = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    WNBD_LOG_LOUD(": Exit");

    return Status;
}
