#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/system.h"

#include "filename.h"

BOX_FORM("takes thing \"the file to rename\"\n"
         "takes name \"its new name\"\n"
         "gives things\n");

int main(void) {
    const file_id_t *file;
    size_t file_count;
    const char *const *new_name;
    size_t name_count;

    if (form_things(0, &file, &file_count) != 0 || form_words(1, &new_name, &name_count) != 0) {
        exit(1);
        return 1;
    }

    char *old_name = name_of_file(file[0]);
    const char *was = old_name ? old_name : "the file";

    if (file_rename(file[0], new_name[0]) != 0) {
        printf("Could not rename %s to %s\n", was, new_name[0]);
        free(old_name);
        exit(1);
        return 1;
    }

    souvenir_things(file, 1);
    printf("%s is now called %s\n", was, new_name[0]);
    free(old_name);
    exit(0);
    return 0;
}
