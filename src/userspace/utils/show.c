#include "box/io.h"
#include "box/ipc.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

int main(void) {
    int argc;
    char argv[16][64];
    receive_args(&argc, argv, 16);

    if (argc < 2) {
        println("Usage: show <filename>");
        exit(1);
        return 1;
    }

    uint32_t matches[16];
    int count = find_file_by_name(argv[1], matches, NULL, 16);

    if (count < 0) { println("Error: Failed to query files"); exit(1); return 1; }
    if (count == 0) { println("Error: File not found"); exit(1); return 1; }

    if (count > 1) {
        println("Multiple files found:");
        for (int i = 0; i < count; i++) {
            file_info_t info;
            if (file_info(matches[i], &info) == 0) {
                printf("  %s [file_id=%u]\n", info.filename, matches[i]);
            }
        }
        println("Error: Ambiguous name (use file_id)");
        exit(1);
        return 1;
    }

    printf("Content of %s:\n", argv[1]);
    println("(File content display not yet implemented)");

    exit(0);
    return 0;
}
