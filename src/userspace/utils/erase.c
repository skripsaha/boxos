#include "box/print.h"
#include "box/form.h"
#include "box/error.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/system.h"

/* Takes words, not things: a trashed file is hidden from every query, so the
 * shell's lookup of a name would never find the one this command exists to
 * delete. The names are looked for in the trash itself, below. */
BOX_FORM("takes words \"the trashed files to erase, or trashed for all of them\"\n");

/* True when the volume still knows the file's name; then the caller releases
 * the metadata with FileMetadataRelease. */
static bool read_name(file_id_t file, FileMetadata *metadata)
{
    return FileMetadataRead(file, metadata) == 0 && metadata->Filename;
}

static int erase_one(file_id_t file, const char *name)
{
    if (delete(file) != 0) {
        printf("Could not erase %s\n", name);
        return 1;
    }
    printf("File deleted: %s\n", name);
    return 0;
}

/* Emptying the trash is not asking for any one file: a file the system keeps
 * (one wearing system or boot, refused to every program that does not) stays
 * in the trash and is said so, and the trash was still emptied of the rest —
 * only a file that could not be erased for any other reason is a failure. */
static int erase_all_trashed(const file_id_t *trashed, int total)
{
    int deleted = 0;
    int kept = 0;
    int failed = 0;
    for (int i = 0; i < total; i++) {
        FileMetadata metadata;
        bool named = read_name(trashed[i], &metadata);
        const char *name = named ? metadata.Filename : "a file in the trash";
        int rc = delete(trashed[i]);
        if (rc == 0) {
            deleted++;
        } else if (box_errno_of(rc) == ERR_PERMISSION_DENIED) {
            printf("%s is kept by the system; it stays in the trash\n", name);
            kept++;
        } else {
            printf("Could not erase %s\n", name);
            failed++;
        }
        if (named) FileMetadataRelease(&metadata);
    }
    if (deleted == 0 && kept == 0 && failed == 0) println("The trash is empty");
    else if (deleted > 0 || failed > 0)
        printf("Erased %d file%s from the trash\n", deleted, deleted == 1 ? "" : "s");
    return failed > 0;
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
        FileMetadata metadata;
        if (!read_name(trashed[i], &metadata)) continue;
        if (strcmp(metadata.Filename, name) == 0) {
            file_id = trashed[i];
            count++;
        }
        FileMetadataRelease(&metadata);
    }

    if (count > 1) {
        printf("%s is in the trash more than once; erase them by their tags\n", name);
        return 1;
    }

    if (count == 0) {
        /* Not in the trash: the look-up by name tells a name nobody has from
         * a file still out of the trash. */
        if (find_file_by_name(name, NULL, NULL, 0) <= 0)
            printf("There is no file called %s\n", name);
        else
            printf("%s is not in the trash; put it there first with trash %s\n", name, name);
        return 1;
    }

    return erase_one(file_id, name);
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
    if (total < 0) { println("Could not look in the trash"); exit(1); return 1; }

    int failed = 0;
    for (size_t i = 0; i < name_count; i++) {
        if (strcmp(names[i], "trashed") == 0)
            failed |= erase_all_trashed(trashed, total);
        else
            failed |= erase_named(trashed, total, names[i]);
    }
    free(trashed);

    exit(failed);
    return failed;
}
