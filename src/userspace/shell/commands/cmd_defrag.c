#include "commands.h"
#include "box/io.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"
#include "box/convert.h"

int cmd_defrag(int argc, char* argv[]) {
    if (argc < 2) {
        println("Usage: defrag <file_id|filename>");
        return -1;
    }

    uint32_t file_id;

    if (is_number(argv[1])) {
        file_id = to_uint(argv[1]);
    } else {
        uint32_t matches[16];
        int count = find_file_by_name(argv[1], matches, NULL, 16);

        if (count <= 0) { println("Error: File not found"); return -1; }
        if (count > 1) { println("Error: Ambiguous filename"); return -1; }

        file_id = matches[0];
    }

    int result = defrag(file_id, 0);

    if (result < 0) {
        println("Error: Defragmentation failed");
        return -1;
    }

    printf("Defragmented file %u, fragmentation score: %u%%\n", file_id, (unsigned)result);
    return 0;
}
