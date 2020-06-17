/*
 * Copyright (c) 2019 SUSE LLC
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#ifndef USERSPACE_H
#define USERSPACE_H 1

#include "driver.h"
#include "driver_extension.h"
#include "scsi_driver_extensions.h"
#include "userspace_shared.h"

// TODO: make this configurable.
#define MAX_IN_FLIGHT_REQUESTS 255

typedef struct _USER_ENTRY {
    LIST_ENTRY                         ListEntry;
    struct _SCSI_DEVICE_INFORMATION*   ScsiInformation;
    ULONG                              BusIndex;
    ULONG                              TargetIndex;
    ULONG                              LunIndex;
    BOOLEAN                            Connected;
    UINT64                             DiskSize;
    UINT16                             BlockSize;
    CONNECTION_INFO                    UserInformation;
} USER_ENTRY, *PUSER_ENTRY;

typedef struct _SCSI_DEVICE_INFORMATION
{
    PWNBD_SCSI_DEVICE           Device;
    PGLOBAL_INFORMATION         GlobalInformation;

    ULONG                       BusIndex;
    ULONG                       TargetIndex;
    ULONG                       LunIndex;
    PINQUIRYDATA                InquiryData;

    PUSER_ENTRY                 UserEntry;
    INT                         Socket;
    INT                         SocketToClose;

    // TODO: rename as PendingReqListHead
    LIST_ENTRY                  RequestListHead;
    KSPIN_LOCK                  RequestListLock;

    // TODO: rename as SubmittedReqListHead
    LIST_ENTRY                  ReplyListHead;
    KSPIN_LOCK                  ReplyListLock;

    KSEMAPHORE                  RequestSemaphore;

    KSEMAPHORE                  DeviceEvent;
    // TODO: rename as DeviceReplySemaphore
    KSEMAPHORE                  DeviceEventReply;
    PVOID                       DeviceRequestThread;
    PVOID                       DeviceReplyThread;
    BOOLEAN                     HardTerminateDevice;
    BOOLEAN                     SoftTerminateDevice;

    WNBD_STATS                  Stats;
    KSPIN_LOCK                  StatsLock;
    PVOID                       ReadPreallocatedBuffer;
    ULONG                       ReadPreallocatedBufferLength;
    PVOID                       WritePreallocatedBuffer;
    ULONG                       WritePreallocatedBufferLength;
} SCSI_DEVICE_INFORMATION, *PSCSI_DEVICE_INFORMATION;

NTSTATUS
WnbdParseUserIOCTL(_In_ PVOID GlobalHandle,
                   _In_ PIRP Irp);

BOOLEAN
WnbdFindConnection(_In_ PGLOBAL_INFORMATION GInfo,
                   _In_ PCONNECTION_INFO Info,
                   _Maybenull_ PUSER_ENTRY* Entry);

NTSTATUS
WnbdCreateConnection(_In_ PGLOBAL_INFORMATION GInfo,
                     _In_ PCONNECTION_INFO Info);

NTSTATUS
WnbdDeleteConnectionEntry(_In_ PUSER_ENTRY Entry);

NTSTATUS
WnbdEnumerateActiveConnections(_In_ PGLOBAL_INFORMATION GInfo,
                               _In_ PIRP Irp);

NTSTATUS
WnbdDeleteConnection(_In_ PGLOBAL_INFORMATION GInfo,
                     _In_ PCONNECTION_INFO Info);

VOID
WnbdInitScsiIds();

#endif
