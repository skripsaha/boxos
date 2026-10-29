#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/system.h"

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

    if (file_rename(file[0], new_name[0]) != 0) {
        println("Error: Failed to rename file");
        exit(1);
        return 1;
    }

    souvenir_things(file, 1);
    println("File renamed");
    exit(0);
    return 0;
}
