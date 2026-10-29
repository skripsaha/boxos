#include "box/print.h"
#include "box/system.h"
#include "box/form.h"

BOX_FORM("");

int main(void) {

    int score = fragmentation();

    if (score < 0) {
        println("Could not read how fragmented the volume is");
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
