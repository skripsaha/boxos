#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/system.h"

#include "filename.h"

BOX_FORM("takes things \"the files to put in the trash\"\n"
         "gives things\n");

int main(void) {
    const file_id_t *files;
    size_t file_count;

    if (form_things(0, &files, &file_count) != 0) {
        exit(1);
        return 1;
    }

    file_id_t *trashed = malloc(file_count * sizeof(file_id_t));
    if (!trashed) {
        println("There is not enough memory to trash these files");
        exit(1);
        return 1;
    }

    size_t done = 0;
    for (size_t i = 0; i < file_count; i++) {
        if (tag_add(files[i], "trashed") == 0) {
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
        free(name);
    }
    free(trashed);

    int failed = done < file_count;
    exit(failed);
    return failed;
}
