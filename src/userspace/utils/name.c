#include "box/print.h"
#include "box/luggage.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

int main(void) {
    int argc = (int)luggage_word_count();

    if (argc < 3) {
        println("Usage: name <oldname> <newname>");
        exit(1);
        return 1;
    }

    uint32_t matches[16];
    int count = find_file_by_name(luggage_word(1), matches, NULL, 16);

    if (count <= 0) { println("Error: File not found"); exit(1); return 1; }
    if (count > 1) { println("Error: Ambiguous filename"); exit(1); return 1; }

    if (file_rename(matches[0], luggage_word(2)) != 0) {
        println("Error: Failed to rename file");
        exit(1);
        return 1;
    }

    println("File renamed");
    exit(0);
    return 0;
}
