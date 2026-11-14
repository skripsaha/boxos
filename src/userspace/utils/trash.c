#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/system.h"

#include "filename.h"
#include "trash_term.h"

BOX_FORM("takes optional things \"the files to put in the trash\"\n"
         "takes optional words \"restore, then the names of the files in the trash to bring back\"\n"
         "gives things\n");

/* restore: each name looked for among the files kept deleted, and the one
 * called so brought back. Says what it could not. Exit 0 when all came back. */
static int restore_named(const char *const *names, size_t name_count) {
    file_id_t *kept  = NULL;
    int        total = query_all_deleted(NULL, NULL, &kept);
    int        failed = total < 0;
    if (failed) println("The trash could not be looked in");

    for (size_t n = 0; n < name_count && !failed; n++) {
        file_id_t found = 0;
        int       count = 0;
        for (int i = 0; i < total; i++) {
            FileMetadata metadata;
            if (FileMetadataRead(kept[i], &metadata) != 0) continue;
            if (metadata.Filename && strcmp(metadata.Filename, names[n]) == 0) {
                found = kept[i];
                count++;
            }
            FileMetadataRelease(&metadata);
        }
        if (count == 0) {
            printf("There is no file called %s in the trash\n", names[n]);
        } else if (count > 1) {
            printf("%s is in the trash %d times; it cannot be told which to bring back\n", names[n], count);
        } else if (file_restore(found) == 0) {
            printf("%s is back\n", names[n]);
            continue;
        } else {
            printf("%s could not be brought back\n", names[n]);
        }
        failed = 1;
    }
    free(kept);
    return failed;
}

int main(void) {
    const file_id_t   *files;
    const char *const *words;
    size_t             file_count, word_count;

    if (form_things(0, &files, &file_count) != 0 || form_words(1, &words, &word_count) != 0) {
        exit(1);
        return 1;
    }
    if (word_count > 0) {
        int failed = strcmp(words[0], "restore") != 0 || word_count == 1 || file_count > 0;
        if (failed) println("trash: files to put in the trash, or restore and the names of files in it");
        else failed = restore_named(words + 1, word_count - 1);
        exit(failed);
        return failed;
    }
    if (file_count == 0) {
        println("trash: say which files to put in the trash");
        exit(1);
        return 1;
    }
    const uint64_t term = TrashTermSeconds();

    file_id_t *trashed = malloc(file_count * sizeof(file_id_t));
    if (!trashed) {
        println("There is not enough memory to trash these files");
        exit(1);
        return 1;
    }

    size_t done = 0;
    for (size_t i = 0; i < file_count; i++) {
        if (file_keep(files[i], term) == 0) {
            trashed[done++] = files[i];
            continue;
        }
        char *name = name_of_file(files[i]);
        printf("Could not move %s to the trash\n", name ? name : "a file");
        free(name);
    }

    souvenir_things(trashed, done);

    if (done > 0) {
        char *name = done == 1 ? name_of_file(trashed[0]) : NULL;
        if (done == 1)
            printf("%s moved to the trash", name ? name : "The file");
        else
            printf("%u files moved to the trash", (unsigned)done);
        if (done < file_count) printf("; %u could not be moved", (unsigned)(file_count - done));
        println("");
        if (term > 0)
            printf("It is kept %u day(s) and then goes by itself; trash restore brings it back\n",
                   (unsigned)(term / TRASH_TERM_SECONDS_A_DAY));
        else
            println("It is gone at once: the hub's trash-days is 0");
        free(name);
    }
    free(trashed);

    int failed = done < file_count;
    exit(failed);
    return failed;
}
