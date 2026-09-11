#include "box/print.h"
#include "box/luggage.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"
#include "box/convert.h"

int main(void) {
    int argc = (int)luggage_word_count();

    if (argc < 2) {
        println("Usage: defrag <file_id|filename>");
        exit(1);
        return 1;
    }

    uint32_t file_id;

    if (is_number(luggage_word(1))) {
        file_id = to_uint(luggage_word(1));
    } else {
        uint32_t matches[16];
        int count = find_file_by_name(luggage_word(1), matches, NULL, 16);

        if (count <= 0) { println("Error: File not found"); exit(1); return 1; }
        if (count > 1) { println("Error: Ambiguous filename"); exit(1); return 1; }

        file_id = matches[0];
    }

    int result = defrag(file_id, 0);

    if (result < 0) {
        println("Error: Defragmentation failed");
        exit(1);
        return 1;
    }

    printf("Defragmented file %u, fragmentation score: %u%%\n", file_id, (unsigned)result);
    exit(0);
    return 0;
}