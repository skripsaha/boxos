#include "box/print.h"
#include "box/luggage.h"
#include "box/memory.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

int main(void) {
    int argc = (int)luggage_word_count();

    if (argc < 2) {
        println("Usage: create <filename> [tags...]");
        exit(1);
        return 1;
    }

    /* Every word after the name is a tag; joined with commas into a list
     * sized to what was typed, so no tag is ever dropped. */
    char *tags = NULL;
    if (argc > 2) {
        size_t total = 0;
        for (int i = 2; i < argc; i++) total += strlen(luggage_word(i)) + 1;
        tags = malloc(total);
        if (!tags) {
            println("Error: no memory for the tag list");
            exit(1);
            return 1;
        }
        size_t pos = 0;
        for (int i = 2; i < argc; i++) {
            size_t tag_len = strlen(luggage_word(i));
            if (i > 2) tags[pos++] = ',';
            memcpy(tags + pos, luggage_word(i), tag_len);
            pos += tag_len;
        }
        tags[pos] = '\0';
    }

    int file_id = create(luggage_word(1), tags);
    free(tags);

    if (file_id < 0) {
        println("Error: Failed to create file");
        exit(1);
        return 1;
    }

    printf("Created file: %s\n", luggage_word(1));
    exit(0);
    return 0;
}
