#include "commands.h"
#include "box/io.h"
#include "box/file.h"
#include "box/string.h"

int find_file_by_name(const char* filename, uint32_t* file_ids,
                      file_info_t* out_infos, size_t max) {
    uint32_t all_files[256];
    int total = query(NULL, all_files, 256);

    if (total < 0) return -1;

    int match_count = 0;
    for (int i = 0; i < total && (size_t)match_count < max; i++) {
        file_info_t info;
        if (file_info(all_files[i], &info) == 0) {
            if (strcmp(info.filename, filename) == 0) {
                file_ids[match_count] = all_files[i];
                if (out_infos) {
                    out_infos[match_count] = info;
                }
                match_count++;
            }
        }
    }

    return match_count;
}

int cmd_tag(int argc, char* argv[]) {
    if (argc < 3) {
        println("Usage: tag <filename> <key:value>");
        return -1;
    }

    const char* filename = argv[1];
    const char* tag = argv[2];

    uint32_t matches[16];
    int count = find_file_by_name(filename, matches, NULL, 16);

    if (count <= 0) {
        println("Error: File not found");
        return -1;
    }

    if (count > 1) {
        println("Error: Ambiguous filename");
        return -1;
    }

    if (tag_add(matches[0], tag) != 0) {
        println("Error: Failed to add tag");
        return -1;
    }

    println("Tag added");
    return 0;
}
