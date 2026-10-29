#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/system.h"

/* Takes words, not things: a trashed file is hidden from every query, so the
 * shell's lookup of a name would never find the one this command exists to
 * delete. The names are looked for in the trash itself, below. */
BOX_FORM("takes words \"the trashed files to erase, or trashed for all of them\"\n");

static void erase_all_trashed(file_id_t *trashed, int total)
{
    int deleted = 0;
    for (int i = 0; i < total; i++) {
        if (delete(trashed[i]) == 0) deleted++;
    }
    printf("Deleted %d trashed files\n", deleted);
}

/* The name is looked for in the trash itself. A look-up by name asks the
 * index the way a listing does, and a listing leaves the trash out - so it
 * never found a trashed file, and the file it did find was refused as not
 * trashed yet: `erase NAME` could delete nothing at all. */
static int erase_named(const file_id_t *trashed, int total, const char *name)
{
    file_id_t file_id = 0;
    int count = 0;
    for (int i = 0; i < total; i++) {
        file_info_t info;
        if (file_info(trashed[i], &info) != 0) continue;
        if (strcmp(info.filename, name) != 0) continue;
        file_id = trashed[i];
        count++;
    }

    if (count > 1) {
        printf("Error: %s is in the trash more than once\n", name);
        return 1;
    }

    if (count == 0) {
        /* Not in the trash: the look-up by name tells a name nobody has from
         * a file still out of the trash. */
        if (find_file_by_name(name, NULL, NULL, 0) <= 0)
            printf("Error: File not found: %s\n", name);
        else
            printf("Error: %s must be trashed first (use 'trash' command)\n", name);
        return 1;
    }

    if (delete(file_id) != 0) {
        printf("Error: Failed to delete %s\n", name);
        return 1;
    }

    printf("File deleted: %s (file %lu)\n", name, file_id);
    return 0;
}

int main(void) {
    const char *const *names;
    size_t name_count;

    if (form_words(0, &names, &name_count) != 0) {
        exit(1);
        return 1;
    }

    file_id_t *trashed = NULL;
    int total = query_all("trashed", &trashed);
    if (total < 0) { println("Error: Failed to query files"); exit(1); return 1; }

    int failed = 0;
    for (size_t i = 0; i < name_count; i++) {
        if (strcmp(names[i], "trashed") == 0)
            erase_all_trashed(trashed, total);
        else
            failed |= erase_named(trashed, total, names[i]);
    }
    free(trashed);

    exit(failed);
    return failed;
}
