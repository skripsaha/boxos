#include "box/io.h"
#include "box/ipc.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

int main(void) {
    int argc;
    char argv[16][64];
    receive_args(&argc, argv, 16);

    if (argc < 2) {
        system_info_t sys;
        if (sysinfo(&sys) != 0) {
            println("Error: Failed to get system info");
            exit(1);
            return 1;
        }

        println("BoxOS System Info:");
        printf("  Version: %s\n", sys.version);
        printf("  Uptime: %u seconds\n", sys.uptime_seconds);
        printf("  Memory Total: %u bytes\n", sys.total_memory);
        printf("  Memory Used: %u bytes\n", sys.used_memory);

        exit(0);
        return 0;
    }

    uint32_t matches[4];
    file_info_t infos[4];
    int count = find_file_by_name(argv[1], matches, infos, 4);

    if (count <= 0) { println("Error: File not found"); exit(1); return 1; }
    if (count > 1) { println("Error: Ambiguous filename"); exit(1); return 1; }

    file_info_t info = infos[0];

    println("File Info:");
    printf("  Filename: %s\n", info.filename);
    printf("  File ID: %u\n", matches[0]);
    printf("  Size: %u bytes\n", (unsigned)info.size);
    printf("  Flags: 0x%x\n", info.flags);

    println("  Tags:");
    for (uint8_t i = 0; i < info.tag_count && i < 5; i++) {
        if (info.tags[i].value[0])
            printf("    %s:%s\n", info.tags[i].key, info.tags[i].value);
        else
            printf("    %s\n", info.tags[i].key);
    }

    exit(0);
    return 0;
}
