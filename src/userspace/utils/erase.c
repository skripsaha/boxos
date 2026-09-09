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

    uint32_t matches[4];
    file_info_t infos[4];
    int count = find_file_by_name(luggage_word(1), matches, infos, 4);

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
