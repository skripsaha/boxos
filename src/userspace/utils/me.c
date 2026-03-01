#include "box/io.h"
#include "box/ipc.h"
#include "box/system.h"

int main(void) {
    int argc;
    char argv[16][64];
    receive_args(&argc, argv, 16);

    system_info_t info;

    if (sysinfo(&info) != 0) {
        println("Error: Failed to get system info");
        exit(1);
        return 1;
    }

    println("BoxOS System Statistics:");
    printf("  Version: %s\n", info.version);
    printf("  Uptime: %u seconds\n", info.uptime_seconds);
    printf("  Total Memory: %u bytes\n", info.total_memory);
    printf("  Used Memory: %u bytes\n", info.used_memory);
    printf("  Free Memory: %u bytes\n", info.total_memory - info.used_memory);

    exit(0);
    return 0;
}
