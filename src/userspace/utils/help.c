#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

#include "box/memory.h"

BOX_FORM("");

static void sort_names(char **names, int count)
{
    for (int i = 1; i < count; i++) {
        char *key = names[i];
        int j = i - 1;
        while (j >= 0 && strcmp(names[j], key) > 0) {
            names[j + 1] = names[j];
            j--;
        }
        names[j + 1] = key;
    }
}

/* Every utility on the volume, however many there are, by its whole name. */
static void list_utilities(void)
{
    file_id_t    *ids      = NULL;
    FileMetadata *metadata = NULL;
    char        **names    = NULL;
    int count = query_all("utility", &ids);
    if (count > 0) {
        metadata = calloc((size_t)count, sizeof(FileMetadata));
        names    = malloc((size_t)count * sizeof(char *));
    }
    if (count > 0 && (!metadata || !names)) count = -1;

    int named = 0;
    for (int i = 0; i < count; i++) {
        if (FileMetadataRead(ids[i], &metadata[i]) != 0 || !metadata[i].Filename) continue;
        if (strcmp(metadata[i].Filename, "display.elf") == 0) continue;
        if (strcmp(metadata[i].Filename, "shell.bin") == 0) continue;
        names[named++] = metadata[i].Filename;
    }

    if (named > 0) {
        sort_names(names, named);
        for (int i = 0; i < named; i++) printf("  %s\n", names[i]);
    } else {
        println("  (no utilities found)");
    }

    for (int i = 0; i < count; i++) FileMetadataRelease(&metadata[i]);
    free(metadata);
    free(names);
    free(ids);
}

int main(void) {

    println("BoxOS Shell v1.0 - Available Commands:");
    println("");
    println("Built-in:");
    println("  use [tags]               Set/clear context tags");
    println("  clear                    Clear the screen");
    println("  exit                     Exit shell");
    println("");
    println("Utilities:");

    list_utilities();

    exit(0);
    return 0;
}
