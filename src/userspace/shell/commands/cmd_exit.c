#include "commands.h"
#include "../shell.h"
#include "box/io.h"

int cmd_exit(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    println("Goodbye!");
    shell_stop();
    return 0;
}
