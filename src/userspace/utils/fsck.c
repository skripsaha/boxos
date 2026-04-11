#include "box/io.h"
#include "box/ipc.h"
#include "box/system.h"

int main(void) {
    int argc;
    char argv[16][64];
    receive_args(&argc, argv, 16);

    int score = fragmentation();

    if (score < 0) {
        println("Error: Failed to get fragmentation score");
        exit(1);
        return 1;
    }

    println("Filesystem Health Check:");
    printf("  Fragmentation: %u%%\n", (unsigned)score);

    if (score == 0) println("  Status: Perfect (no fragmentation)");
    else if (score < 10) println("  Status: Excellent");
    else if (score < 30) println("  Status: Good");
    else if (score < 60) println("  Status: Fair (consider defragmentation)");
    else println("  Status: Poor (defragmentation recommended)");

    exit(0);
    return 0;
}
