#include "box/print.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

#include "box/memory.h"

int main(void) {

    println("BoxOS Shell v1.0 - Available Commands:");
    println("");
    println("Built-in:");
    println("  use [tags]               Set/clear context tags");
    println("  clear                    Clear the screen");
    println("  exit                     Exit shell");
    println("");
    println("Utilities:");

    uint32_t    *s_file_ids = NULL;
    file_info_t *s_infos    = NULL;
    int count = query_all("utility", &s_file_ids);
    if (count > 0) s_infos = malloc((size_t)count * sizeof(file_info_t));
    if (count > 0 && !s_infos) count = -1;

    if (count > 0) {
        for (int i = 0; i < count; i++) {
            if (file_info(s_file_ids[i], &s_infos[i]) != 0) {
                s_infos[i].filename[0] = '\0';
            }
        }

        for (int i = 1; i < count; i++) {
            file_info_t key = s_infos[i];
            int j = i - 1;
            while (j >= 0 &&
                   s_infos[j].filename[0] != '\0' &&
                   key.filename[0] != '\0' &&
                   strcmp(s_infos[j].filename, key.filename) > 0) {
                s_infos[j + 1] = s_infos[j];
                j--;
            }
            s_infos[j + 1] = key;
        }

        for (int i = 0; i < count; i++) {
            if (s_infos[i].filename[0] == '\0') continue;
            if (strcmp(s_infos[i].filename, "display.elf") == 0) continue;
            if (strcmp(s_infos[i].filename, "shell.bin") == 0) continue;
            printf("  %s\n", s_infos[i].filename);
        }
    } else {
        println("  (no utilities found)");
    }

    exit(0);
    return 0;
}