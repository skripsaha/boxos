#include "box/io.h"
#include "box/ipc.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

int main(void) {
    int argc;
    char argv[16][64];
    receive_args(&argc, argv, 16);

    println("BoxOS Shell v1.0 - Available Commands:");
    println("");
    println("Built-in:");
    println("  use [tags]               Set/clear context tags");
    println("  clear                    Clear the screen");
    println("  exit                     Exit shell");
    println("");
    println("Utilities:");

    uint32_t file_ids[256];
    int count = query("utility", file_ids, 256);

    if (count > 0) {
        file_info_t infos[256];
        for (int i = 0; i < count && i < 256; i++) {
            if (file_info(file_ids[i], &infos[i]) != 0) {
                infos[i].filename[0] = '\0';
            }
        }

        for (int i = 0; i < count - 1; i++) {
            for (int j = 0; j < count - i - 1; j++) {
                if (infos[j].filename[0] != '\0' &&
                    infos[j + 1].filename[0] != '\0' &&
                    strcmp(infos[j].filename, infos[j + 1].filename) > 0) {
                    file_info_t tmp = infos[j];
                    infos[j] = infos[j + 1];
                    infos[j + 1] = tmp;
                }
            }
        }

        for (int i = 0; i < count; i++) {
            if (infos[i].filename[0] == '\0') continue;
            if (strcmp(infos[i].filename, "display.elf") == 0) continue;
            if (strcmp(infos[i].filename, "shell.bin") == 0) continue;
            printf("  %s\n", infos[i].filename);
        }
    } else {
        println("  (no utilities found)");
    }

    exit(0);
    return 0;
}
