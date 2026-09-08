#include "box/print.h"
#include "box/luggage.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

int main(void) {
    int argc = (int)luggage_word_count();

    if (argc < 3) {
        println("Usage: untag <filename> <key>");
        exit(1);
        return 1;
    }

    uint32_t matches[16];
    int count = find_file_by_name(luggage_word(1), matches, NULL, 16);

    if (count <= 0) { println("Error: File not found"); exit(1); return 1; }
    if (count > 1) { println("Error: Ambiguous filename"); exit(1); return 1; }

    if (tag_remove(matches[0], luggage_word(2)) != 0) {
        println("Error: Failed to remove tag");
        exit(1);
        return 1;
    }

    println("Tag removed");
    exit(0);
    return 0;
}
