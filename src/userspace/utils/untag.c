#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/system.h"

BOX_FORM("takes things \"the files to take tags off\"\n"
         "takes words \"the tags to take off\"\n"
         "gives things\n");

int main(void) {
    const file_id_t *files;
    size_t file_count;
    const char *const *tags;
    size_t tag_count;

    if (form_things(0, &files, &file_count) != 0 || form_words(1, &tags, &tag_count) != 0) {
        exit(1);
        return 1;
    }

    size_t untagged = 0;
    for (size_t i = 0; i < file_count; i++) {
        bool whole = true;
        for (size_t t = 0; t < tag_count; t++) {
            if (tag_remove(files[i], tags[t]) != 0) {
                printf("Error: Failed to remove tag %s from file %lu\n", tags[t], files[i]);
                whole = false;
                break;
            }
        }
        if (!whole) break;
        untagged++;
    }

    souvenir_things(files, untagged);

    if (untagged < file_count) {
        exit(1);
        return 1;
    }

    const char *what = tag_count == 1 ? "Tag" : "Tags";
    if (file_count == 1)
        printf("%s removed\n", what);
    else
        printf("%s removed from %u files\n", what, (unsigned)file_count);
    exit(0);
    return 0;
}
