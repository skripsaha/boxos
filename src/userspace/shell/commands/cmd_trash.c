#include "commands.h"
#include "box/io.h"
#include "box/file.h"
#include "box/string.h"


int cmd_trash(int argc, char* argv[]) {
    if (argc < 2) {
        println("Usage: trash <filename>");
        return -1;
    }

    uint32_t matches[16];
    int count = find_file_by_name(argv[1], matches, NULL, 16);

    if (count <= 0) { println("Error: File not found"); return -1; }
    if (count > 1) { println("Error: Ambiguous filename"); return -1; }

    if (tag_add(matches[0], "trashed") != 0) {
        println("Error: Failed to trash file");
        return -1;
    }

    println("File moved to trash");
    return 0;
}
