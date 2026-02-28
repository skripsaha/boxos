#include "commands.h"
#include "box/io.h"
#include "box/file.h"
#include "box/string.h"


int cmd_name(int argc, char* argv[]) {
    if (argc < 3) {
        println("Usage: name <oldname> <newname>");
        return -1;
    }

    uint32_t matches[16];
    int count = find_file_by_name(argv[1], matches, NULL, 16);

    if (count <= 0) { println("Error: File not found"); return -1; }
    if (count > 1) { println("Error: Ambiguous filename"); return -1; }

    if (file_rename(matches[0], argv[2]) != 0) {
        println("Error: Failed to rename file");
        return -1;
    }

    println("File renamed");
    return 0;
}
