#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/memory.h"
#include "box/system.h"

#include "filename.h"

BOX_FORM("takes thing \"the file to defragment\"\n"
         "gives things\n");

int main(void) {
    const file_id_t *file;
    size_t file_count;

    if (form_things(0, &file, &file_count) != 0) {
        exit(1);
        return 1;
    }

    file_id_t file_id = file[0];
    char *name = name_of_file(file_id);
    const char *called = name ? name : "the file";
    int result = defrag(file_id);

    if (result < 0) {
        printf("Could not defragment %s\n", called);
        free(name);
        exit(1);
        return 1;
    }

    souvenir_things(&file_id, 1);
    printf("Defragmented %s, fragmentation score: %u%%\n", called, (unsigned)result);
    free(name);
    exit(0);
    return 0;
}
