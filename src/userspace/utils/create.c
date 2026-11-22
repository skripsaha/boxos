#include "box/print.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

BOX_FORM("takes name \"the new file's name\"\n"
         "takes optional words \"its tags\"\n"
         "gives things\n");

int main(void) {
    const char *const *name;
    size_t name_count;
    const char *const *words;
    size_t word_count;

    if (form_words(0, &name, &name_count) != 0 || form_words(1, &words, &word_count) != 0) {
        exit(1);
        return 1;
    }

    /* Every word is a tag; joined with commas into a list sized to what was
     * typed, so no tag is ever dropped. */
    char *tags = NULL;
    if (word_count > 0) {
        size_t total = 0;
        for (size_t i = 0; i < word_count; i++) total += strlen(words[i]) + 1;
        tags = malloc(total);
        if (!tags) {
            println("Error: no memory for the tag list");
            exit(1);
            return 1;
        }
        size_t pos = 0;
        for (size_t i = 0; i < word_count; i++) {
            size_t tag_len = strlen(words[i]);
            if (i > 0) tags[pos++] = ',';
            memcpy(tags + pos, words[i], tag_len);
            pos += tag_len;
        }
        tags[pos] = '\0';
    }

    int64_t file_id = create(PERSON_TAGS, name[0], tags);
    free(tags);

    if (file_id < 0) {
        printf("Could not create %s\n", name[0]);
        exit(1);
        return 1;
    }

    file_id_t made = (file_id_t)file_id;
    souvenir_things(&made, 1);
    printf("Created file: %s\n", name[0]);
    exit(0);
    return 0;
}
