#include "commands.h"
#include "box/io.h"
#include "box/system.h"

int cmd_reboot(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    println("Rebooting system...");
    reboot();
    return 0;
}
