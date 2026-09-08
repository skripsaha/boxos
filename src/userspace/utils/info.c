#include "box/print.h"
#include "box/luggage.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

int main(void) {
    int argc = (int)luggage_word_count();

    if (argc < 2) {
        system_info_t sys;
        if (sysinfo(&sys) != 0) {
            printf("%colorError:%color Failed to get system info\n",
                   COLOR_RED, COLOR_DEFAULT);
            exit(1);
            return 1;
        }

        uint64_t up_ms = sys.uptime_ns / 1000000ULL;
        printf("%color%s%color  uptime %u ms  procs %u\n",
               COLOR_CYAN, sys.version, COLOR_DEFAULT,
               (unsigned)up_ms, sys.process_count);
        printf("  RAM:  %u MB used / %u MB total\n",
               (unsigned)(sys.used_memory  >> 20),
               (unsigned)(sys.total_memory >> 20));
        printf("  CPUs: %u (%u K, %u App)%s\n",
               sys.cpu_total, sys.cpu_k_cores, sys.cpu_app_cores,
               sys.multicore_active ? " AMP" : "");

        exit(0);
        return 0;
    }

    uint32_t matches[4];
    file_info_t infos[4];
    int count = find_file_by_name(luggage_word(1), matches, infos, 4);

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
