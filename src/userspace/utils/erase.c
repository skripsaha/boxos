#include "box/print.h"
#include "box/luggage.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/system.h"

int main(void) {
    int argc = (int)luggage_word_count();

    if (argc < 2) {
        println("Usage: erase <filename|trashed>");
        exit(1);
        return 1;
    }

    if (strcmp(luggage_word(1), "trashed") == 0) {
        /* Ask for the trashed files by their tag — a plain listing leaves
         * them out on purpose, so it was the one list that could never hold
         * what this command deletes. */
        uint32_t *trashed = NULL;
        int total = query_all("trashed", &trashed);

        if (total < 0) { println("Error: Failed to query files"); exit(1); return 1; }

        int deleted = 0;
        for (int i = 0; i < total; i++) {
            if (delete(trashed[i]) == 0) deleted++;
        }
        free(trashed);

        printf("Deleted %d trashed files\n", deleted);
        exit(0);
        return 0;
    }

    /* The name is looked for in the trash itself. A look-up by name asks the
     * index the way a listing does, and a listing leaves the trash out — so
     * it never found a trashed file, and the file it did find was refused as
     * not trashed yet: `erase NAME` could delete nothing at all. */
    const char *name = luggage_word(1);
    uint32_t *trashed = NULL;
    int total = query_all("trashed", &trashed);

    if (total < 0) { println("Error: Failed to query files"); exit(1); return 1; }

    uint32_t file_id = 0;
    int count = 0;
    for (int i = 0; i < total; i++) {
        file_info_t info;
        if (file_info(trashed[i], &info) != 0) continue;
        if (strcmp(info.filename, name) != 0) continue;
        file_id = trashed[i];
        count++;
    }
    free(trashed);

    if (count > 1) { println("Error: Ambiguous filename"); exit(1); return 1; }

    if (count == 0) {
        /* Not in the trash: the look-up by name tells a name nobody has from
         * a file still out of the trash. */
        if (find_file_by_name(name, NULL, NULL, 0) <= 0)
            println("Error: File not found");
        else
            println("Error: File must be trashed first (use 'trash' command)");
        exit(1);
        return 1;
    }

    if (delete(file_id) != 0) {
        println("Error: Failed to delete file");
        exit(1);
        return 1;
    }

    printf("File deleted: %s (file %u)\n", name, file_id);
    exit(0);
    return 0;
}
