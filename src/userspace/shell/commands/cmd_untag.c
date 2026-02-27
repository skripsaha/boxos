#include "commands.h"
#include "box/io.h"
#include "box/file.h"
#include "box/string.h"

extern int find_file_by_name(const char* filename, uint32_t* file_ids,
                             file_info_t* out_infos, size_t max);

int cmd_untag(int argc, char* argv[]) {
    if (argc < 3) {
        println("Usage: untag <filename> <key>");
        return -1;
    }

    uint32_t matches[16];
    int count = find_file_by_name(argv[1], matches, NULL, 16);

    if (count <= 0) { println("Error: File not found"); return -1; }
    if (count > 1) { println("Error: Ambiguous filename"); return -1; }

    if (tag_remove(matches[0], argv[2]) != 0) {
        println("Error: Failed to remove tag");
        return -1;
    }

    println("Tag removed");
    return 0;
}
