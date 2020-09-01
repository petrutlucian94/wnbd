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
#include "userspace_shared.h"
#include "wvbd.h"

void
PrintSyntax();

DWORD
CmdUnmap(PCHAR InstanceName);

DWORD
CmdStats(PCHAR InstanceName);

DWORD
CmdMap(
    PCHAR InstanceName,
    PCHAR HostName,
    PCHAR PortName,
    PCHAR ExportName,
    UINT64 DiskSize,
    BOOLEAN MustNegotiate,
    BOOLEAN ReadOnly);

DWORD
CmdList();

DWORD
CmdRaiseLogLevel(UINT32 LogLevel);

#ifdef __cplusplus
}
#endif
