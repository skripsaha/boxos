#include "commands.h"
#include "box/io.h"

int cmd_say(int argc, char* argv[]) {
    if (argc < 2) {
        println("Usage: say <text...>");
        return -1;
    }

    for (int i = 1; i < argc; i++) {
        if (i > 1) print(" ");
        print(argv[i]);
    }
    println("");

    return 0;
}
