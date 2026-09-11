#include "commands.h"
#include "../shell.h"
#include "box/system.h"

int cmd_exit(int argc, char *argv[])
{
    (void)argc;
    (void)argv;
    ShellStop();
    return 0;
}