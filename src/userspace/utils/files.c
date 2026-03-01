#include "box/io.h"
#include "box/ipc.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

int main(void) {
    int argc;
    char argv[16][64];
    receive_args(&argc, argv, 16);

    char query_tags[256];
    query_tags[0] = '\0';

    if (argc > 1) {
        size_t pos = 0;
        for (int i = 1; i < argc && pos < 250; i++) {
            if (i > 1) query_tags[pos++] = ',';
            size_t len = strlen(argv[i]);
            if (pos + len < 250) {
                memcpy(query_tags + pos, argv[i], len);
                pos += len;
            }
        }
        query_tags[pos] = '\0';
    }

    uint32_t file_ids[256];
    int count = query((argc > 1) ? query_tags : NULL, file_ids, 256);

    if (count < 0) { println("Error: Failed to query files"); exit(1); return 1; }
    if (count > 256) { println("Warning: Too many files, showing first 256"); count = 256; }
    if (count == 0) { println("No files found"); exit(0); return 0; }

    println("Files:");

    file_info_t file_infos[256];
    for (int i = 0; i < count; i++) {
        if (file_info(file_ids[i], &file_infos[i]) != 0) {
            file_infos[i].filename[0] = '\0';
        }
    }

    for (int i = 0; i < count - 1; i++) {
        for (int j = 0; j < count - i - 1; j++) {
            if (file_infos[j].filename[0] != '\0' &&
                file_infos[j + 1].filename[0] != '\0' &&
                strcmp(file_infos[j].filename, file_infos[j + 1].filename) > 0) {
                file_info_t tmp = file_infos[j];
                file_infos[j] = file_infos[j + 1];
                file_infos[j + 1] = tmp;

                uint32_t tmp_id = file_ids[j];
                file_ids[j] = file_ids[j + 1];
                file_ids[j + 1] = tmp_id;
            }
        }
    }

    for (int i = 0; i < count; i++) {
        if (file_ids[i] == 0 || file_infos[i].filename[0] == '\0') continue;

        file_info_t info = file_infos[i];

        print(info.filename);

        size_t name_len = strlen(info.filename);
        for (size_t j = name_len; j < 32; j++) print(" ");

        print(" [");

        int printed_tags = 0;
        for (uint8_t t = 0; t < info.tag_count && t < 5; t++) {
            if (printed_tags > 0) print(", ");

            if (info.tags[t].type == 0) {
                printf("%s:%s", info.tags[t].key, info.tags[t].value);
            } else {
                print(info.tags[t].key);
            }
            printed_tags++;
        }

        println("]");
    }

    exit(0);
    return 0;
}
