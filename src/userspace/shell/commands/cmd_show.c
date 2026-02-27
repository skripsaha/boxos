#include "commands.h"
#include "box/io.h"
#include "box/file.h"
#include "box/string.h"

extern int find_file_by_name(const char* filename, uint32_t* file_ids,
                             file_info_t* out_infos, size_t max);

int cmd_show(int argc, char* argv[]) {
    if (argc < 2) {
        println("Usage: show <filename>");
        return -1;
    }

    uint32_t matches[16];
    int count = find_file_by_name(argv[1], matches, NULL, 16);

    if (count < 0) { println("Error: Failed to query files"); return -1; }
    if (count == 0) { println("Error: File not found"); return -1; }

    if (count > 1) {
        println("Multiple files found:");
        for (int i = 0; i < count; i++) {
            file_info_t info;
            if (file_info(matches[i], &info) == 0) {
                printf("  %s [file_id=%u]\n", info.filename, matches[i]);
            }
        }
        println("Error: Ambiguous name (use file_id)");
        return -1;
    }

    printf("Content of %s:\n", argv[1]);
    println("(File content display not yet implemented)");

    return 0;
}
