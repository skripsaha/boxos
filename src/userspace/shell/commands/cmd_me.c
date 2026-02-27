#include "commands.h"
#include "box/io.h"
#include "box/system.h"

int cmd_me(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    system_info_t info;

    if (sysinfo(&info) != 0) {
        println("Error: Failed to get system info");
        return -1;
    }

    println("BoxOS System Statistics:");
    printf("  Version: %s\n", info.version);
    printf("  Uptime: %u seconds\n", info.uptime_seconds);
    printf("  Total Memory: %u bytes\n", info.total_memory);
    printf("  Used Memory: %u bytes\n", info.used_memory);
    printf("  Free Memory: %u bytes\n", info.total_memory - info.used_memory);

    return 0;
}
