#ifndef WVBD_DISPATCH_H
#define WVBD_DISPATCH_H 1

#include "common.h"
#include "userspace.h"

// TODO: consider moving this to util.h
NTSTATUS LockUsermodeBuffer(PIRP Irp,
                            PVOID Buffer, UINT32 BufferSize, BOOLEAN Writeable,
                            PVOID* OutBuffer);

NTSTATUS WvbdDispatchRequest(
    PIRP Irp,
    PSCSI_DEVICE_INFORMATION DeviceInfo,
    PWVBD_IOCTL_FETCH_REQ_COMMAND Command);

NTSTATUS WvbdHandleResponse(
    PIRP Irp,
    PSCSI_DEVICE_INFORMATION DeviceInfo,
    PWVBD_IOCTL_SEND_RSP_COMMAND Command);

#endif // WVBD_DISPATCH_H
