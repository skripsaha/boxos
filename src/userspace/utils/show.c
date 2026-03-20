#include "box/io.h"
#include "box/ipc.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

int main(void) {
    int argc;
    char argv[16][64];
    receive_args(&argc, argv, 16);

    if (argc < 2) {
        println("Usage: show <filename>");
        exit(1);
        return 1;
    }

    uint32_t matches[16];
    int count = find_file_by_name(argv[1], matches, NULL, 16);

    if (count < 0) { println("Error: Failed to query files"); exit(1); return 1; }
    if (count == 0) { println("Error: File not found"); exit(1); return 1; }

    if (count > 1) {
        println("Multiple files found:");
        for (int i = 0; i < count; i++) {
            file_info_t info;
            if (file_info(matches[i], &info) == 0) {
                printf("  %s [file_id=%u]\n", info.filename, matches[i]);
            }
        }
        println("Error: Ambiguous name (use file_id)");
        exit(1);
        return 1;
    }

    uint32_t file_id = matches[0];
    file_info_t info;
    if (file_info(file_id, &info) != 0) {
        println("Error: Failed to get file info");
        exit(1);
        return 1;
    }

    printf("Content of %s (%lu bytes):\n", info.filename, (unsigned long)info.size);
    println("----------------------------------------");

    // Read and display file content in chunks
    char buffer[176];  // max fread chunk size
    size_t total_read = 0;
    size_t bytes_to_read = (info.size > 4096) ? 4096 : info.size;  // limit display

    while (total_read < bytes_to_read) {
        size_t chunk = bytes_to_read - total_read;
        if (chunk > sizeof(buffer)) chunk = sizeof(buffer);

        int result = fread(file_id, total_read, buffer, chunk);
        if (result < 0) {
            println("Error: Failed to read file content");
            exit(1);
            return 1;
        }
        if (result == 0) break;

        // Print the chunk (handle binary safely)
        for (int i = 0; i < result; i++) {
            char c = buffer[i];
            // Print printable characters, escape others
            if (c >= 32 && c < 127) {
                char s[2] = {c, 0};
                print(s);
            } else if (c == '\n' || c == '\r' || c == '\t') {
                char s[2] = {c, 0};
                print(s);
            } else {
                printf("\\x%02x", (unsigned char)c);
            }
        }

        total_read += result;
        if ((size_t)result < chunk) break;
    }

    if (info.size > 4096) {
        println("\n----------------------------------------");
        printf("(Truncated: showing first 4096 of %lu bytes)\n", (unsigned long)info.size);
    }

    println("");
    exit(0);
    return 0;
}
