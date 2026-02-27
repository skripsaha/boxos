#include "commands.h"
#include "box/io.h"
#include "box/system.h"

int cmd_bye(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    println("Shutting down system...");
    shutdown();
    return 0;
}
