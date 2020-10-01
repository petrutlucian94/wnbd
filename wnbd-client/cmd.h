/*
 * Copyright (c) 2019 SUSE LLC
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <windows.h>
#include <winioctl.h>
#include <shlobj.h>
#include <stdio.h>
#include <stdlib.h>
#include <winioctl.h>
#include <ntddscsi.h>
#include <setupapi.h>
#include <string.h>
#include <process.h>

/* WNBD Defines */
#include "wnbd.h"

#define WNBD_CLI_OWNER_NAME "wnbd-client"
#define WNBD_DEFAULT_RM_TIMEOUT_MS 30 * 1000
#define WNBD_DEFAULT_RM_RETRY_INTERVAL_MS 2000

void
PrintSyntax();

DWORD
CmdUnmap(PCHAR InstanceName, BOOLEAN HardRemove);

DWORD
CmdStats(PCHAR InstanceName);

DWORD
CmdMap(
    PCHAR InstanceName,
    PCHAR HostName,
    DWORD PortNumber,
    PCHAR ExportName,
    UINT64 DiskSize,
    UINT32 BlockSize,
    BOOLEAN MustNegotiate,
    BOOLEAN ReadOnly);

DWORD
CmdList();

DWORD
CmdRaiseLogLevel(UINT32 LogLevel);

DWORD
CmdVersion();

#ifdef __cplusplus
}
#endif
