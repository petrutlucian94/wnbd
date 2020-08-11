#ifndef WVBD_SHARED_H
#define WVBD_SHARED_H

#include <wvbd_ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WVBD_MIN_DISPATCHER_THREAD_COUNT 1
#define WVBD_MAX_DISPATCHER_THREAD_COUNT 255
#define WVBD_LOG_MESSAGE_MAX_SIZE 4096

typedef enum
{
    WvbdLogLevelCritical = 0,
    WvbdLogLevelError = 1,
    WvbdLogLevelWarning = 2,
    WvbdLogLevelInfo = 3,
    WvbdLogLevelDebug = 4,
    WvbdLogLevelTrace = 5
} WvbdLogLevel;

typedef struct _WVBD_STATS {
    UINT64 TotalReceivedRequests;
    UINT64 TotalSubmittedRequests;
    UINT64 TotalReceivedReplies;
    UINT64 UnsubmittedRequests;
    UINT64 PendingSubmittedRequests;
    UINT64 ReadErrors;
    UINT64 WriteErrors;
    UINT64 FlushErrors;
    UINT64 UnmapErrors;
    UINT64 InvalidRequests;
    UINT64 TotalRWRequests;
    UINT64 TotalReadBlocks;
    UINT64 TotalWrittenBlocks;
} WVBD_STATS, *PWVBD_STATS;

typedef struct _WVBD_INTERFACE WVBD_INTERFACE;
typedef struct _WVBD_DEVICE
{
    HANDLE Handle;
    WVBD_DISK_HANDLE DiskHandle;
    PVOID Context;
    WVBD_PROPERTIES Properties;
    WvbdLogLevel LogLevel;
    const WVBD_INTERFACE *Interface;
    BOOLEAN Stopping;
    BOOLEAN Stopped;
    BOOLEAN Started;
    HANDLE* DispatcherThreads;
    UINT32 DispatcherThreadsCount;
    WVBD_STATS Stats;
} WVBD_DEVICE, *PWVBD_DEVICE;

typedef VOID (*ReadFunc)(
    PWVBD_DEVICE Device,
    UINT64 RequestHandle,
    PVOID Buffer,
    UINT64 BlockAddress,
    UINT32 BlockCount,
    BOOLEAN ForceUnitAccess);
typedef VOID (*WriteFunc)(
    PWVBD_DEVICE Device,
    UINT64 RequestHandle,
    PVOID Buffer,
    UINT64 BlockAddress,
    UINT32 BlockCount,
    BOOLEAN ForceUnitAccess);
typedef VOID (*FlushFunc)(
    PWVBD_DEVICE Device,
    UINT64 RequestHandle,
    UINT64 BlockAddress,
    UINT32 BlockCount);
typedef VOID (*UnmapFunc)(
    PWVBD_DEVICE Device,
    UINT64 RequestHandle,
    PWVBD_UNMAP_DESCRIPTOR Descriptors,
    UINT32 Count);
typedef VOID (*LogMessageFunc)(
    PWVBD_DEVICE Device,
    WvbdLogLevel LogLevel,
    const char* Message,
    const char* FileName,
    UINT32 Line,
    const char* FunctionName);

// The following IO callbacks should be implemented by the consumer.
// As an alternative, the underlying *Ioctl* functions may be used directly
// in order to retrieve and process requests.
typedef struct _WVBD_INTERFACE
{
    ReadFunc Read;
    WriteFunc Write;
    FlushFunc Flush;
    UnmapFunc Unmap;
    LogMessageFunc LogMessage;
    // TODO: add a logger callback
    VOID* Reserved[14];
} WVBD_INTERFACE, *PWVBD_INTERFACE;

DWORD WvbdCreate(
    const PWVBD_PROPERTIES Properties,
    const PWVBD_INTERFACE Interface,
    PVOID Context,
    WvbdLogLevel LogLevel,
    PWVBD_DEVICE* PDevice);
DWORD WvbdRemove(PWVBD_DEVICE Device);
VOID WvbdClose(PWVBD_DEVICE Device);


void WvbdSetSenseEx(
    PWVBD_STATUS Status,
    UINT8 SenseKey,
    UINT8 Asc,
    UINT64 Info);
void WvbdSetSense(PWVBD_STATUS Status, UINT8 SenseKey, UINT8 Asc);

DWORD WvbdStartDispatcher(PWVBD_DEVICE Device, DWORD ThreadCount);
DWORD WvbdWaitDispatcher(PWVBD_DEVICE Device);
DWORD WvbdSendResponse(
    PWVBD_DEVICE Device,
    PWVBD_IO_RESPONSE Response,
    PVOID DataBuffer,
    UINT32 DataBufferSize);

HANDLE WvbdOpenDevice();
DWORD WvbdIoctlCreate(
    HANDLE Device,
    PWVBD_PROPERTIES Properties,
    PWVBD_DISK_HANDLE DiskHandle);
DWORD WvbdIoctlRemove(HANDLE Device, const char* InstanceName);
DWORD WvbdIoctlRemoveEx(const char* InstanceName);

// The disk handle should be handled carefully in order to avoid delayed replies
// from being submitted to other disks after being remapped.
DWORD WvbdIoctlFetchRequest(
    HANDLE Device,
    WVBD_DISK_HANDLE DiskHandle,
    PWVBD_IO_REQUEST Request,
    PVOID DataBuffer,
    UINT32 DataBufferSize);
DWORD WvbdIoctlSendResponse(
    HANDLE Device,
    WVBD_DISK_HANDLE DiskHandle,
    PWVBD_IO_RESPONSE Response,
    PVOID DataBuffer,
    UINT32 DataBufferSize);

static inline const CHAR* WvbdLogLevelToStr(WvbdLogLevel LogLevel) {
    switch(LogLevel)
    {
        case WvbdLogLevelCritical:
            return "CRITICAL";
        case WvbdLogLevelError:
            return "ERROR";
        case WvbdLogLevelWarning:
            return "WARNING";
        case WvbdLogLevelInfo:
            return "INFO";
        case WvbdLogLevelDebug:
            return "DEBUG";
        case WvbdLogLevelTrace:
        default:
            return "TRACE";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* WVBD_SHARED_H */
