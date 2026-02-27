#include "commands.h"
#include "box/io.h"
#include "box/system.h"

int cmd_fsck(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    int score = fragmentation();

    if (score < 0) {
        println("Error: Failed to get fragmentation score");
        return -1;
    }

    println("Filesystem Health Check:");
    printf("  Fragmentation: %u%%\n", (unsigned)score);

    if (score == 0) println("  Status: Perfect (no fragmentation)");
    else if (score < 10) println("  Status: Excellent");
    else if (score < 30) println("  Status: Good");
    else if (score < 60) println("  Status: Fair (consider defragmentation)");
    else println("  Status: Poor (defragmentation recommended)");

    return 0;
}
