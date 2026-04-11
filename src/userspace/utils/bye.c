#include "box/io.h"
#include "box/ipc.h"
#include "box/system.h"

int main(void) {
    int argc;
    char argv[16][64];
    receive_args(&argc, argv, 16);

    println("Shutting down system...");
    shutdown();

    exit(0);
    return 0;
}
