#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/system.h"

BOX_FORM("takes things \"the files to put in the trash\"\n"
         "gives things\n");

int main(void) {
    const file_id_t *files;
    size_t file_count;

    if (form_things(0, &files, &file_count) != 0) {
        exit(1);
        return 1;
    }

    size_t trashed = 0;
    for (size_t i = 0; i < file_count; i++) {
        if (tag_add(files[i], "trashed") != 0) {
            printf("Error: Failed to trash file %lu\n", files[i]);
            break;
        }
        trashed++;
    }

    souvenir_things(files, trashed);

    if (trashed < file_count) {
        exit(1);
        return 1;
    }

    if (file_count == 1)
        println("File moved to trash");
    else
        printf("%u files moved to trash\n", (unsigned)file_count);
    exit(0);
    return 0;
}
