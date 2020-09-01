/*
 * Copyright (c) 2019 SUSE LLC
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#include "cmd.h"
#include <userspace_shared.h>
#include <string>

bool arg_to_bool(char* arg) {
    return !_stricmp(arg, "1") ||
           !_stricmp(arg, "yes") ||
           !_stricmp(arg, "y");
}

int main(int argc, PCHAR argv[])
{
    PCHAR Command;
    PCHAR InstanceName = NULL;
    PCHAR PortName = NULL;
    PCHAR ExportName = NULL;
    PCHAR HostName = NULL;

    Command = argv[1];

    if ((argc >= 6) && !strcmp(Command, "map")) {
        InstanceName = argv[2];
        HostName = argv[3];
        PortName = argv[4];
        ExportName = argv[5];
        BOOLEAN MustNegotiate = TRUE;
        BOOLEAN ReadOnly = FALSE;

        if (argc > 6) {
            MustNegotiate = arg_to_bool(argv[6]);
        }
        if (argc > 7) {
            ReadOnly = arg_to_bool(argv[7]);
        }
        CmdMap(InstanceName, HostName, PortName, ExportName, 50000,
                MustNegotiate, ReadOnly);
    } else if (argc == 3 && !strcmp(Command, "unmap")) {
        InstanceName = argv[2];
        CmdUnmap(InstanceName);
    } else if (argc == 3 && !strcmp(Command, "stats")) {
        InstanceName = argv[2];
        CmdStats(InstanceName);
    } else if (argc == 2 && !strcmp(Command, "list")) {
        return CmdList();
    } else if (argc == 3 && !strcmp(Command, "set-debug")) {
        int Value = atoi(argv[2]);
        CmdRaiseLogLevel(!!Value);
    } else {
        PrintSyntax();
        return -1;
    }
    
    return 0;
}