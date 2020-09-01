#include "wvbd_dispatch.h"
#include "util.h"
#include "srb_helper.h"
#include "debug.h"
#include "scsi_function.h"
#include "scsi_trace.h"

inline int
ScsiOpToWvbdReqType(int ScsiOp)
{
    switch (ScsiOp) {
    case SCSIOP_READ6:
    case SCSIOP_READ:
    case SCSIOP_READ12:
    case SCSIOP_READ16:
        return WvbdReqTypeRead;
    case SCSIOP_WRITE6:
    case SCSIOP_WRITE:
    case SCSIOP_WRITE12:
    case SCSIOP_WRITE16:
        return WvbdReqTypeWrite;
    case SCSIOP_UNMAP:
        return WvbdReqTypeUnmap;
    case SCSIOP_SYNCHRONIZE_CACHE:
    case SCSIOP_SYNCHRONIZE_CACHE16:
        return WvbdReqTypeFlush;
    default:
        return WvbdReqTypeUnknown;
    }
}

NTSTATUS LockUsermodeBuffer(PIRP Irp, PVOID Buffer, UINT32 BufferSize, BOOLEAN Writeable,
                            PVOID* OutBuffer)
{
    NTSTATUS Status = 0;
    __try
    {
        if (Writeable)
            ProbeForWrite(Buffer, BufferSize, 1);
        else
            ProbeForRead(Buffer, BufferSize, 1);

        PMDL Mdl = IoAllocateMdl(
            Buffer,
            BufferSize,
            0 != Irp->MdlAddress,
            FALSE,
            Irp);
        if (!Mdl)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }

        MmProbeAndLockPages(Mdl, UserMode, IoWriteAccess);

        *OutBuffer = MmGetSystemAddressForMdlSafe(Mdl, NormalPagePriority);
        if (!*OutBuffer)
        {
            Status = STATUS_INSUFFICIENT_RESOURCES;
            __leave;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Status = GetExceptionCode();
    }

    return Status;
}

NTSTATUS WvbdDispatchRequest(
    PIRP Irp,
    PSCSI_DEVICE_INFORMATION DeviceInfo,
    PWVBD_IOCTL_FETCH_REQ_COMMAND Command)
{
    PVOID Buffer;
    NTSTATUS Status = 0;
    PWVBD_IO_REQUEST Request = &Command->Request;

    Status = LockUsermodeBuffer(
        Irp, Command->DataBuffer, Command->DataBufferSize, 1, &Buffer);
    if (Status)
        return Status;

    static UINT64 RequestHandle = 0;

    // We're looping through the requests until we manage to dispatch one.
    // Unsupported requests as well as most errors will be hidden from the caller.
    while (!DeviceInfo->HardTerminateDevice) {
        PVOID WaitObjects[2];
        WaitObjects[0] = &DeviceInfo->DeviceEvent;
        WaitObjects[1] = &DeviceInfo->TerminateEvent;
        KeWaitForMultipleObjects(2, WaitObjects, WaitAny, Executive, KernelMode,
                                 FALSE, NULL, NULL);
        PLIST_ENTRY RequestEntry = ExInterlockedRemoveHeadList(
            &DeviceInfo->RequestListHead,
            &DeviceInfo->RequestListLock);

        if (DeviceInfo->HardTerminateDevice) {
            break;
        }
        if (!RequestEntry) {
            continue;
        }

        // TODO: consider moving this part to a helper function.
        PSRB_QUEUE_ELEMENT Element = CONTAINING_RECORD(RequestEntry, SRB_QUEUE_ELEMENT, Link);
        Element->Tag = InterlockedIncrement64(&(LONG64)RequestHandle);
        Element->Srb->DataTransferLength = 0;
        PCDB Cdb = (PCDB)&Element->Srb->Cdb;

        RtlZeroMemory(Request, sizeof(WVBD_IO_REQUEST));
        WvbdRequestType RequestType = ScsiOpToWvbdReqType(Cdb->AsByte[0]);
        WNBD_LOG_LOUD("Processing request. Address: %p Tag: 0x%llx Type: %d",
                      Element->Srb, Element->Tag, RequestType);
        // TODO: check if the device supports the requested operation
        switch(RequestType) {
        case WvbdReqTypeRead:
        case WvbdReqTypeWrite:
            Request->RequestType = RequestType;
            Request->RequestHandle = Element->Tag;
            break;
        // TODO: flush/unmap
        default:
            Element->Srb->DataTransferLength = 0;
            Element->Srb->SrbStatus = SRB_STATUS_INVALID_REQUEST;
            StorPortNotification(RequestComplete,
                                 Element->DeviceExtension,
                                 Element->Srb);
            ExFreePool(Element);
            InterlockedDecrement64(&DeviceInfo->Stats.UnsubmittedIORequests);
            continue;
        }

        switch(RequestType) {
        case WvbdReqTypeRead:
            Request->Cmd.Read.BlockAddress =
                Element->StartingLbn / DeviceInfo->UserEntry->BlockSize;;
            Request->Cmd.Read.BlockCount =
                Element->ReadLength / DeviceInfo->UserEntry->BlockSize;
            break;
        case WvbdReqTypeWrite:
            Request->Cmd.Write.BlockAddress =
                Element->StartingLbn / DeviceInfo->UserEntry->BlockSize;
            Request->Cmd.Write.BlockCount =
                Element->ReadLength / DeviceInfo->UserEntry->BlockSize;
            if (Element->ReadLength > Command->DataBufferSize) {
                // TODO: insert the request back in the queue and remove it
                // from the reply list. The semaphore should also be incremented.
                return STATUS_BUFFER_TOO_SMALL;
            }

            PVOID SrbBuffer;
            if (StorPortGetSystemAddress(Element->DeviceExtension,
                                         Element->Srb, &SrbBuffer)) {
                // TODO: consider moving this part to a helper function.
                Element->Srb->SrbStatus = SRB_STATUS_INTERNAL_ERROR;
                StorPortNotification(
                    RequestComplete,
                    Element->DeviceExtension,
                    Element->Srb);
                ExFreePool(Element);
                InterlockedDecrement64(&DeviceInfo->Stats.UnsubmittedIORequests);
                continue;
            }

            RtlCopyMemory(Buffer, SrbBuffer, Element->ReadLength);
            break;
        }

        ExInterlockedInsertTailList(
            &DeviceInfo->ReplyListHead,
            &Element->Link, &DeviceInfo->ReplyListLock);
        InterlockedIncrement64(&DeviceInfo->Stats.PendingSubmittedIORequests);
        InterlockedDecrement64(&DeviceInfo->Stats.UnsubmittedIORequests);
        // We managed to find a supported request, we can now exit the loop
        // and pass it forward.
        break;
    }

    if (DeviceInfo->HardTerminateDevice) {
        Request->RequestType = WvbdReqTypeDisconnect;
        Status = 0;
    }

    return Status;
}

NTSTATUS WvbdHandleResponse(
    PIRP Irp,
    PSCSI_DEVICE_INFORMATION DeviceInfo,
    PWVBD_IOCTL_SEND_RSP_COMMAND Command)
{
    PSRB_QUEUE_ELEMENT Element = NULL;
    NTSTATUS Status = STATUS_SUCCESS;
    PVOID SrbBuff = NULL, LockedUserBuff = NULL;

    PWVBD_IO_RESPONSE Response = &Command->Response;

    PLIST_ENTRY ItemLink, ItemNext;
    KIRQL Irql = { 0 };
    KeAcquireSpinLock(&DeviceInfo->ReplyListLock, &Irql);
    LIST_FORALL_SAFE(&DeviceInfo->ReplyListHead, ItemLink, ItemNext) {
        Element = CONTAINING_RECORD(ItemLink, SRB_QUEUE_ELEMENT, Link);
        if (Element->Tag == Response->RequestHandle) {
            RemoveEntryList(&Element->Link);
            break;
        }
        Element = NULL;
    }
    KeReleaseSpinLock(&DeviceInfo->ReplyListLock, Irql);
    if (!Element) {
        WNBD_LOG_ERROR("Received reply with no matching request tag: 0x%llx",
            Response->RequestHandle);
        Status = STATUS_NOT_FOUND;
        goto Exit;
    }

    ULONG StorResult;
    if (!Element->Aborted) {
        // We need to avoid accessing aborted or already completed SRBs.
        PCDB Cdb = (PCDB)&Element->Srb->Cdb;
        WvbdRequestType RequestType = ScsiOpToWvbdReqType(Cdb->AsByte[0]);
        WNBD_LOG_LOUD("Received reply header for %d %p 0x%llx.",
                      RequestType, Element->Srb, Element->Tag);

        if (IsReadSrb(Element->Srb)) {
            StorResult = StorPortGetSystemAddress(Element->DeviceExtension, Element->Srb, &SrbBuff);
            if (STOR_STATUS_SUCCESS != StorResult) {
                WNBD_LOG_ERROR("Could not get SRB %p 0x%llx data buffer. Error: %d.",
                               Element->Srb, Element->Tag, StorResult);
                Element->Srb->SrbStatus = SRB_STATUS_INTERNAL_ERROR;
                Status = STATUS_INTERNAL_ERROR;
                goto Exit;
            }
        }
    } else {
        WNBD_LOG_WARN("Received reply header for aborted request: %p 0x%llx.",
                      Element->Srb, Element->Tag);
    }

    if (!Response->Status.ScsiStatus && IsReadSrb(Element->Srb)) {
        Status = LockUsermodeBuffer(
            Irp, Command->DataBuffer, Command->DataBufferSize, FALSE, &LockedUserBuff);
        if (Status)
            goto Exit;

        // TODO: compare data buffer size with the read length
        if (!Element->Aborted) {
            RtlCopyMemory(SrbBuff, LockedUserBuff, Element->ReadLength);
        }
    }
    if (Response->Status.ScsiStatus) {
        Element->Srb->DataTransferLength = 0;
        Element->Srb->SrbStatus = SetSrbStatus(Element->Srb, &Response->Status);
    }
    else {
        // TODO: rename ReadLength to DataLength
        Element->Srb->DataTransferLength = Element->ReadLength;
        Element->Srb->SrbStatus = SRB_STATUS_SUCCESS;
    }

    InterlockedIncrement64(&DeviceInfo->Stats.TotalReceivedIOReplies);
    InterlockedDecrement64(&DeviceInfo->Stats.PendingSubmittedIORequests);
    // TODO: consider dropping this counter, relying on the request list instead.
    InterlockedDecrement(&DeviceInfo->Device->OutstandingIoCount);

    if (Element->Aborted) {
        InterlockedIncrement64(&DeviceInfo->Stats.CompletedAbortedIORequests);
    }

Exit:
    if (Element) {
        if (!Element->Aborted) {
            WNBD_LOG_LOUD(
                "Notifying StorPort of completion of %p status: 0x%x(%s)",
                Element->Srb, Element->Srb->SrbStatus,
                WnbdToStringSrbStatus(Element->Srb->SrbStatus));
            StorPortNotification(RequestComplete, Element->DeviceExtension,
                                 Element->Srb);
        }
        ExFreePool(Element);
    }

    return Status;
}
