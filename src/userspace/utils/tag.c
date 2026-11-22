#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/system.h"

#include "filename.h"

BOX_FORM("takes things \"the files to tag\"\n"
         "takes words \"the tags they get\"\n"
         "gives things\n");

static bool give_tags(file_id_t file, const char *const *tags, size_t tag_count)
{
    for (size_t t = 0; t < tag_count; t++) {
        if (tag_add(PERSON_TAGS, file, tags[t]) == 0) continue;
        char *name = name_of_file(file);
        printf("Could not give the tag %s to %s\n", tags[t], name ? name : "a file");
        free(name);
        return false;
    }
    return true;
}

int main(void) {
    const file_id_t *files;
    size_t file_count;
    const char *const *tags;
    size_t tag_count;

    if (form_things(0, &files, &file_count) != 0 || form_words(1, &tags, &tag_count) != 0) {
        exit(1);
        return 1;
    }

    file_id_t *tagged = malloc(file_count * sizeof(file_id_t));
    if (!tagged) {
        println("There is not enough memory to tag these files");
        exit(1);
        return 1;
    }

    size_t done = 0;
    for (size_t i = 0; i < file_count; i++) {
        if (give_tags(files[i], tags, tag_count)) tagged[done++] = files[i];
    }

    souvenir_things(tagged, done);

    const char *what = tag_count == 1 ? "Tag" : "Tags";
    if (done > 0) {
        char *name = done == 1 ? name_of_file(tagged[0]) : NULL;
        if (done == 1)
            printf("%s added to %s", what, name ? name : "the file");
        else
            printf("%s added to %u files", what, (unsigned)done);
        if (done < file_count) printf("; %u could not be tagged", (unsigned)(file_count - done));
        println("");
        free(name);
    }
    free(tagged);

    int failed = done < file_count;
    exit(failed);
    return failed;
}
