#include "box/io.h"
#include "box/ipc.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

int main(void) {
    int argc;
    char argv[16][64];
    receive_args(&argc, argv, 16);

    if (argc < 3) {
        println("Usage: tag <filename> <key:value>");
        exit(1);
        return 1;
    }

    uint32_t matches[16];
    int count = find_file_by_name(argv[1], matches, NULL, 16);

    if (count <= 0) { println("Error: File not found"); exit(1); return 1; }
    if (count > 1) { println("Error: Ambiguous filename"); exit(1); return 1; }

    if (tag_add(matches[0], argv[2]) != 0) {
        println("Error: Failed to add tag");
        exit(1);
        return 1;
    }

    println("Tag added");
    exit(0);
    return 0;
}
