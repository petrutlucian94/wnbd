/*
 * Copyright (c) 2019 SUSE LLC
 *
 * Licensed under LGPL-2.1 (see LICENSE)
 */

#include "cmd.h"
#include <string>

int main(int argc, const char** argv)
{
    return Client().execute(argc, argv);
}
