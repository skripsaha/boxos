#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/system.h"

#include "filename.h"

BOX_FORM("takes things \"the files to take tags off\"\n"
         "takes words \"the tags to take off\"\n"
         "gives things\n");

typedef enum {
    WORE_NONE,
    TOOK_OFF,
    COULD_NOT
} Outcome;

/* A tag is worn when the file has it by its whole spelling or by its key:
 * "status" is worn by "status:done". */
static bool wears(const char *worn, const char *word)
{
    size_t length = strlen(word);
    while (worn && *worn) {
        const char *end = worn;
        while (*end && *end != ',') end++;
        size_t spelling = (size_t)(end - worn);
        const char *colon = worn;
        while (colon < end && *colon != ':') colon++;
        size_t key = (size_t)(colon - worn);
        if ((spelling == length && memcmp(worn, word, length) == 0) ||
            (key == length && memcmp(worn, word, length) == 0))
            return true;
        worn = *end ? end + 1 : end;
    }
    return false;
}

static Outcome take_tags_off(file_id_t file, const char *const *tags, size_t tag_count)
{
    FileMetadata metadata;
    if (FileMetadataRead(PERSON_TAGS, file, &metadata) != 0) {
        println("Could not look at a file to take its tags off");
        return COULD_NOT;
    }

    Outcome outcome = WORE_NONE;
    for (size_t t = 0; t < tag_count && outcome != COULD_NOT; t++) {
        if (!wears(metadata.Tags, tags[t])) continue;
        if (tag_remove(PERSON_TAGS, file, tags[t]) == 0) {
            outcome = TOOK_OFF;
            continue;
        }
        printf("Could not take the tag %s off %s\n", tags[t], metadata.Filename);
        outcome = COULD_NOT;
    }
    FileMetadataRelease(&metadata);
    return outcome;
}

static void say_result(file_id_t last_removed, file_id_t last_bare, size_t tag_count,
                       size_t removed, size_t bare)
{
    const char *what = tag_count == 1 ? "Tag" : "Tags";
    const char *none = tag_count == 1 ? "it" : "any of them";

    if (removed == 0 && bare == 1) {
        char *name = name_of_file(last_bare);
        printf("%s does not wear %s\n", name ? name : "The file", tag_count == 1 ? "the tag" : "those tags");
        free(name);
    } else if (removed == 0 && bare > 1) {
        printf("None of the %u files wear %s\n", (unsigned)bare, tag_count == 1 ? "the tag" : "those tags");
    } else if (removed > 0) {
        char *name = removed == 1 ? name_of_file(last_removed) : NULL;
        if (removed == 1)
            printf("%s removed from %s", what, name ? name : "the file");
        else
            printf("%s removed from %u files", what, (unsigned)removed);
        if (bare > 0) printf("; %u did not wear %s", (unsigned)bare, none);
        println("");
        free(name);
    }
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

    file_id_t *wore = malloc(file_count * sizeof(file_id_t));
    if (!wore) {
        println("There is not enough memory to take these tags off");
        exit(1);
        return 1;
    }

    size_t removed = 0, bare = 0, failed = 0;
    file_id_t last_bare = 0;
    for (size_t i = 0; i < file_count; i++) {
        Outcome outcome = take_tags_off(files[i], tags, tag_count);
        if (outcome == TOOK_OFF) {
            wore[removed++] = files[i];
        } else if (outcome == WORE_NONE) {
            bare++;
            last_bare = files[i];
        } else {
            failed++;
        }
    }

    souvenir_things(wore, removed);
    say_result(removed ? wore[removed - 1] : 0, last_bare, tag_count, removed, bare);
    free(wore);

    int status = failed > 0;
    exit(status);
    return status;
}
