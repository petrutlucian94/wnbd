#ifndef WVBD_IOCTL_H
#define WVBD_IOCTL_H

#include <userspace_shared.h>

#define WVBD_REGISTRY_KEY "SYSTEM\\CurrentControlSet\\Services\\wnbd"

#define IOCTL_WVBD_PING \
    CTL_CODE(FILE_DEVICE_WNBD, USER_WNBD_IOCTL_START+10, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_WVBD_CREATE \
    CTL_CODE(FILE_DEVICE_WNBD, USER_WNBD_IOCTL_START+11, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_WVBD_REMOVE \
    CTL_CODE(FILE_DEVICE_WNBD, USER_WNBD_IOCTL_START+12, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_WVBD_FETCH_REQ \
    CTL_CODE(FILE_DEVICE_WNBD, USER_WNBD_IOCTL_START+13, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_WVBD_SEND_RSP \
    CTL_CODE(FILE_DEVICE_WNBD, USER_WNBD_IOCTL_START+14, METHOD_BUFFERED, FILE_WRITE_ACCESS)
#define IOCTL_WVBD_LIST \
    CTL_CODE(FILE_DEVICE_WNBD, USER_WNBD_IOCTL_START+15, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_WVBD_STATS \
    CTL_CODE(FILE_DEVICE_WNBD, USER_WNBD_IOCTL_START+16, METHOD_BUFFERED, FILE_READ_ACCESS)
#define IOCTL_WVBD_RELOAD_CONFIG \
    CTL_CODE(FILE_DEVICE_WNBD, USER_WNBD_IOCTL_START+17, METHOD_BUFFERED, FILE_WRITE_ACCESS)


#define MAX_OWNER_LENGTH 16

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
    CHAR Hostname[MAX_NAME_LENGTH];
    UINT32 PortNumber;
    CHAR ExportName[MAX_NAME_LENGTH];
    // Skip NBD negotiation and jump directly to the
    // transmission phase.
    BOOLEAN SkipNegotiation;
} NBD_CONNECTION_PROPERTIES, *PNBD_CONNECTION_PROPERTIES;

typedef struct
{
    UINT32 ReadOnly:1;
    UINT32 FlushSupported:1;
    UINT32 UnmapSupported:1;
    UINT32 UnmapAnchorSupported:1;
    // Connect to an NBD server. If disabled, IO requests and replies will
    // be submitted through the IOCTL_WVBD_FETCH_REQ/IOCTL_WVBD_SEND_RSP
    // DeviceIoControl commands.
    UINT32 UseNbd:1;
    UINT32 Reserved: 27;
} WVBD_FLAGS, *PWVBD_FLAGS;

typedef struct
{
    // Unique disk identifier
    CHAR InstanceName[MAX_NAME_LENGTH];
    // If no serial number is provided, the instance name will be used.
    // This will be exposed through the VPD page.
    CHAR SerialNumber[MAX_NAME_LENGTH];
    // Optional string used to identify the owner of this disk
    // (e.g. can be the project name).
    CHAR Owner[MAX_OWNER_LENGTH];
    WVBD_FLAGS Flags;
    UINT64 BlockCount;
    UINT32 BlockSize;
    UINT32 MaxUnmapDescCount;
    UINT32 MaxTransferLength;
    // Maximum number of pending IO requests at any given time.
    UINT32 MaxOutstandingIO;
    // The userspace process associated with this device.
    INT Pid;
    NBD_CONNECTION_PROPERTIES NbdProperties;
    UINT64 Reserved[32];
} WVBD_PROPERTIES, *PWVBD_PROPERTIES;

typedef struct
{
    WVBD_PROPERTIES Properties;
    BOOLEAN Disconnecting;
    USHORT BusNumber;
    USHORT TargetId;
    USHORT Lun;
    UINT64 Reserved[16];
} WVBD_CONNECTION_INFO, *PWVBD_CONNECTION_INFO;

typedef struct
{
    UINT32 Count;
    UINT32 ElementSize;
    WVBD_CONNECTION_INFO Connections[1];
} WVBD_CONNECTION_LIST, *PWVBD_CONNECTION_LIST;

typedef struct
{
    INT64 TotalReceivedIORequests;
    INT64 TotalSubmittedIORequests;
    INT64 TotalReceivedIOReplies;
    INT64 UnsubmittedIORequests;
    INT64 PendingSubmittedIORequests;
    INT64 AbortedSubmittedIORequests;
    INT64 AbortedUnsubmittedIORequests;
    INT64 CompletedAbortedIORequests;
    INT64 Reserved[16];
} WVBD_DRV_STATS, *PWVBD_DRV_STATS;

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
    UINT64 Reserved[4];
} WVBD_IO_REQUEST, *PWVBD_IO_REQUEST;

typedef struct
{
    UINT64 RequestHandle;
    WvbdRequestType RequestType;
    WVBD_STATUS Status;
    UINT64 Reserved[4];
} WVBD_IO_RESPONSE, *PWVBD_IO_RESPONSE;

typedef struct
{
    ULONG IoControlCode;
} WVBD_IOCTL_BASE_COMMAND, *PWVBD_IOCTL_BASE_COMMAND;

typedef WVBD_IOCTL_BASE_COMMAND WVBD_IOCTL_PING_COMMAND;
typedef PWVBD_IOCTL_BASE_COMMAND PWVBD_IOCTL_PING_COMMAND;

typedef WVBD_IOCTL_BASE_COMMAND WVBD_IOCTL_LIST_COMMAND;
typedef PWVBD_IOCTL_BASE_COMMAND PWVBD_IOCTL_LIST_COMMAND;

typedef WVBD_IOCTL_BASE_COMMAND WVBD_IOCTL_RELOAD_CONFIG_COMMAND;
typedef PWVBD_IOCTL_BASE_COMMAND PWVBD_IOCTL_RELOAD_CONFIG_COMMAND;

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
    CHAR InstanceName[MAX_NAME_LENGTH];
} WVBD_IOCTL_STATS_COMMAND, *PWVBD_IOCTL_STATS_COMMAND;

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
