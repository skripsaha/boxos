#include "box/print.h"
#include "box/form.h"
#include "box/file.h"
#include "box/system.h"

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
    int result = defrag(file_id);

    if (result < 0) {
        println("Error: Defragmentation failed");
        exit(1);
        return 1;
    }

    souvenir_things(&file_id, 1);
    printf("Defragmented file %lu, fragmentation score: %u%%\n", file_id, (unsigned)result);
    exit(0);
    return 0;
}
