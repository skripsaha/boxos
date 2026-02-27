#include "commands.h"
#include "../shell.h"
#include "box/io.h"
#include "box/file.h"
#include "box/string.h"

int cmd_create(int argc, char* argv[]) {
    if (argc < 2) {
        println("Usage: create <filename> [tags...]");
        return -1;
    }

    char tags_buf[256];
    tags_buf[0] = '\0';
    size_t pos = 0;

    for (int i = 2; i < argc && pos < 250; i++) {
        if (i > 2) tags_buf[pos++] = ',';
        size_t tag_len = strlen(argv[i]);
        if (pos + tag_len < 250) {
            memcpy(tags_buf + pos, argv[i], tag_len);
            pos += tag_len;
        }
    }
    tags_buf[pos] = '\0';

    int file_id = create(argv[1], (argc > 2) ? tags_buf : NULL);

    if (file_id < 0) {
        println("Error: Failed to create file");
        return -1;
    }

    printf("Created file: %s\n", argv[1]);
    return 0;
}
