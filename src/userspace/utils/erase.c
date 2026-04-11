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
        println("Usage: erase <filename|trashed>");
        exit(1);
        return 1;
    }

    if (strcmp(argv[1], "trashed") == 0) {
        uint32_t all_files[256];
        int total = query(NULL, all_files, 256);

        if (total < 0) { println("Error: Failed to query files"); exit(1); return 1; }

        int deleted = 0;
        for (int i = 0; i < total; i++) {
            file_info_t info;
            if (file_info(all_files[i], &info) == 0) {
                if (info.flags & 0x02) {
                    if (delete(all_files[i]) == 0) deleted++;
                }
            }
        }

        printf("Deleted %d trashed files\n", deleted);
        exit(0);
        return 0;
    }

    uint32_t matches[4];
    file_info_t infos[4];
    int count = find_file_by_name(argv[1], matches, infos, 4);

    if (count <= 0) { println("Error: File not found"); exit(1); return 1; }
    if (count > 1) { println("Error: Ambiguous filename"); exit(1); return 1; }

    if (!(infos[0].flags & FILE_FLAG_TRASHED)) {
        println("Error: File must be trashed first (use 'trash' command)");
        exit(1);
        return 1;
    }

    if (delete(matches[0]) != 0) {
        println("Error: Failed to delete file");
        exit(1);
        return 1;
    }

    println("File deleted");
    exit(0);
    return 0;
}
