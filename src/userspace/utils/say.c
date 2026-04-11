#include "box/io.h"
#include "box/ipc.h"
#include "box/system.h"

int main(void) {
    int argc;
    char argv[16][64];
    receive_args(&argc, argv, 16);

    if (argc < 2) {
        println("Usage: say <text...>");
        exit(1);
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        if (i > 1) print(" ");
        print(argv[i]);
    }
    println("");

    exit(0);
    return 0;
}
