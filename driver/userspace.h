/*
 * Copyright (c) 2019 SUSE LLC
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#ifndef USERSPACE_H
#define USERSPACE_H 1

#include "driver.h"
#include "nbd_protocol.h"
#include "wnbd_ioctl.h"
#include "scsi_driver_extensions.h"

// TODO: make this configurable. 1024 is the Storport default.
#define WNBD_MAX_IN_FLIGHT_REQUESTS 1024
#define WNBD_PREALLOC_BUFF_SZ (WNBD_DEFAULT_MAX_TRANSFER_LENGTH + sizeof(NBD_REQUEST))

// The connection id provided to the user is meant to be opaque. We're currently
// using the disk address, but that might change.
#define WNBD_CONNECTION_ID_FROM_ADDR(PathId, TargetId, Lun) \
    ((1 << 24 | (PathId) << 16) | ((TargetId) << 8) | (Lun))

typedef struct _WNBD_SCSI_DEVICE
{
    LIST_ENTRY                  ListEntry;

    BOOLEAN                     Connected;
    WNBD_PROPERTIES             Properties;
    WNBD_CONNECTION_ID          ConnectionId;

    USHORT                      Bus;
    USHORT                      Target;
    USHORT                      Lun;

    PINQUIRYDATA                InquiryData;

    INT                         Socket;
    INT                         SocketToClose;
    ERESOURCE                   SocketLock;

    // TODO: rename as PendingReqListHead
    LIST_ENTRY                  RequestListHead;
    KSPIN_LOCK                  RequestListLock;

    // TODO: rename as SubmittedReqListHead
    LIST_ENTRY                  ReplyListHead;
    KSPIN_LOCK                  ReplyListLock;

    KSEMAPHORE                  DeviceEvent;
    PVOID                       DeviceRequestThread;
    PVOID                       DeviceReplyThread;
    BOOLEAN                     HardTerminateDevice;
    BOOLEAN                     SoftTerminateDevice;
    KEVENT                      TerminateEvent;
    // The rundown protection provides device reference counting, preventing
    // it from being deallocated while still being accessed. This is
    // especially important for IO dispatching.
    EX_RUNDOWN_REF              RundownProtection;

    WNBD_DRV_STATS              Stats;
    PVOID                       ReadPreallocatedBuffer;
    ULONG                       ReadPreallocatedBufferLength;
    PVOID                       WritePreallocatedBuffer;
    ULONG                       WritePreallocatedBufferLength;
} WNBD_SCSI_DEVICE, *PWNBD_SCSI_DEVICE;

NTSTATUS
WnbdParseUserIOCTL(_In_ PWNBD_EXTENSION DeviceExtension,
                   _In_ PIRP Irp);

NTSTATUS
WnbdCreateConnection(_In_ PWNBD_EXTENSION DeviceExtension,
                     _In_ PWNBD_PROPERTIES Properties,
                     _In_ PWNBD_CONNECTION_INFO ConnectionInfo);

NTSTATUS
WnbdEnumerateActiveConnections(_In_ PWNBD_EXTENSION DeviceExtension,
                               _In_ PIRP Irp);

NTSTATUS
WnbdDeleteConnection(_In_ PWNBD_EXTENSION DeviceExtension,
                     _In_ PCHAR InstanceName);

VOID
WnbdInitScsiIds();

#endif
