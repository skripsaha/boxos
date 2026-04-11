#include "commands.h"
#include "box/io.h"

int cmd_clear(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    clear();
    return 0;
}