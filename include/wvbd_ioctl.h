#ifndef WVBD_IOCTL_H
#define WVBD_IOCTL_H

#include <userspace_shared.h>

#define STRING_OVERFLOWS(Str, MaxLen) (strlen(Str + 1) > MaxLen)

#define IOCTL_WVBD_CREATE      CTL_CODE(FILE_DEVICE_WNBD, USER_WNBD_IOCTL_START+11, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_WVBD_REMOVE      CTL_CODE(FILE_DEVICE_WNBD, USER_WNBD_IOCTL_START+12, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_WVBD_FETCH_REQ   CTL_CODE(FILE_DEVICE_WNBD, USER_WNBD_IOCTL_START+13, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_WVBD_SEND_RSP    CTL_CODE(FILE_DEVICE_WNBD, USER_WNBD_IOCTL_START+14, METHOD_BUFFERED, FILE_WRITE_ACCESS)

typedef enum
{
    WvbdReqTypeUnknown = 0,
    WvbdReqTypeRead = 1,
    WvbdReqTypeWrite = 2,
    WvbdReqTypeFlush = 3,
    WvbdReqTypeUnmap = 4,
    WvbdReqTypeDisconnect = 5
} WvbdRequestType;

typedef UINT64 WVBD_DISK_HANDLE;
typedef WVBD_DISK_HANDLE *PWVBD_DISK_HANDLE;

typedef struct
{
    UINT8 ScsiStatus;
    UINT8 SenseKey;
    UINT8 ASC;
    UINT8 ASCQ;
    UINT64 Information;
    UINT64 ReservedCSI;
    UINT32 ReservedSKS;
    UINT32 ReservedFRU:8;
    UINT32 InformationValid:1;
} WVBD_STATUS, *PWVBD_STATUS;

typedef struct
{
    CHAR InstanceName[MAX_NAME_LENGTH];
    CHAR SerialNumber[MAX_NAME_LENGTH];
    UINT64 BlockCount;
    UINT32 BlockSize;
    UINT32 ReadOnly:1;
    UINT32 CacheSupported:1;
    UINT32 UnmapSupported:1;
    UINT32 UnmapAnchorSupported:1;
    UINT32 MaxUnmapDescCount;
    UINT32 MaxTransferLength;
    UINT32 MaxOutstandingIO;
} WVBD_PROPERTIES, *PWVBD_PROPERTIES;

typedef struct
{
    UINT64 BlockAddress;
    UINT32 BlockCount;
    UINT32 Reserved;
} WVBD_UNMAP_DESCRIPTOR, *PWVBD_UNMAP_DESCRIPTOR;

typedef struct
{
    UINT64 RequestHandle;
    WvbdRequestType RequestType;
    union
    {
        struct
        {
            UINT64 BlockAddress;
            UINT32 BlockCount;
            UINT32 ForceUnitAccess:1;
            UINT32 Reserved:31;
        } Read;
        struct
        {
            UINT64 BlockAddress;
            UINT32 BlockCount;
            UINT32 ForceUnitAccess:1;
            UINT32 Reserved:31;
        } Write;
        struct
        {
            UINT64 BlockAddress;
            UINT32 BlockCount;
            UINT32 Reserved;
        } Flush;
        struct
        {
            UINT32 Count;
            UINT32 Anchor:1;
            UINT32 Reserved:31;
        } Unmap;
    } Cmd;
} WVBD_IO_REQUEST, *PWVBD_IO_REQUEST;

typedef struct
{
    UINT64 RequestHandle;
    WvbdRequestType RequestType;
    WVBD_STATUS Status;
} WVBD_IO_RESPONSE, *PWVBD_IO_RESPONSE;

typedef struct
{
    ULONG IoControlCode;
    WVBD_PROPERTIES Properties;
} WVBD_IOCTL_CREATE_COMMAND, *PWVBD_IOCTL_CREATE_COMMAND;

typedef struct
{
    ULONG IoControlCode;
    CHAR InstanceName[MAX_NAME_LENGTH];
} WVBD_IOCTL_REMOVE_COMMAND, *PWVBD_IOCTL_REMOVE_COMMAND;

typedef struct
{
    ULONG IoControlCode;
    WVBD_IO_REQUEST Request;
    WVBD_DISK_HANDLE DiskHandle;
    PVOID DataBuffer;
    UINT32 DataBufferSize;
} WVBD_IOCTL_FETCH_REQ_COMMAND, *PWVBD_IOCTL_FETCH_REQ_COMMAND;

typedef struct
{
    ULONG IoControlCode;
    WVBD_IO_RESPONSE Response;
    WVBD_DISK_HANDLE DiskHandle;
    PVOID DataBuffer;
    UINT32 DataBufferSize;
} WVBD_IOCTL_SEND_RSP_COMMAND, *PWVBD_IOCTL_SEND_RSP_COMMAND;

static inline const CHAR* WvbdRequestTypeToStr(WvbdRequestType RequestType) {
    switch(RequestType)
    {
        case WvbdReqTypeRead:
            return "READ";
        case WvbdReqTypeWrite:
            return "WRITE";
        case WvbdReqTypeFlush:
            return "FLUSH";
        case WvbdReqTypeUnmap:
            return "UNMAP";
        case WvbdReqTypeDisconnect:
            return "DISCONNECT";
        default:
            return "UNKNOWN";
    }
}

#endif // WVBD_IOCTL_H
