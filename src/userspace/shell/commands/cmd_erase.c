#include "commands.h"
#include "box/io.h"
#include "box/file.h"
#include "box/string.h"

extern int find_file_by_name(const char* filename, uint32_t* file_ids,
                             file_info_t* out_infos, size_t max);

int cmd_erase(int argc, char* argv[]) {
    if (argc < 2) {
        println("Usage: erase <filename|trashed>");
        return -1;
    }

    if (strcmp(argv[1], "trashed") == 0) {
        uint32_t all_files[256];
        int total = query(NULL, all_files, 256);

        if (total < 0) { println("Error: Failed to query files"); return -1; }

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
        return 0;
    }

    uint32_t matches[4];
    file_info_t infos[4];
    int count = find_file_by_name(argv[1], matches, infos, 4);

    if (count <= 0) { println("Error: File not found"); return -1; }
    if (count > 1) { println("Error: Ambiguous filename"); return -1; }

    int has_trashed_tag = 0;
    for (uint8_t i = 0; i < infos[0].tag_count; i++) {
        if (infos[0].tags[i].type == 1 && strcmp(infos[0].tags[i].key, "trashed") == 0) {
            has_trashed_tag = 1;
            break;
        }
    }

    if (!has_trashed_tag) {
        println("Error: File must be trashed first (use 'trash' command)");
        return -1;
    }

    if (delete(matches[0]) != 0) {
        println("Error: Failed to delete file");
        return -1;
    }

    println("File deleted");
    return 0;
}
