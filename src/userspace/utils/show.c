#include "box/print.h"
#include "box/luggage.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

/* One byte as `show` gives it: a printable one, '\n', '\r' and '\t' as they
 * are, any other as \xNN — spelled here, not by printf: boxlib's converts no
 * field width (print.h — %s %d %u %x %X %c %p %%), so "%02x" went out as
 * written, and a byte's escape with it. */
static void ShowByte(char c)
{
    static const char hex[] = "0123456789abcdef";

    if ((c >= 32 && c < 127) || c == '\n' || c == '\r' || c == '\t')
    {
        char s[2] = {c, 0};
        print(s);
        return;
    }
    char s[5] = {'\\', 'x', hex[(unsigned char)c >> 4], hex[(unsigned char)c & 0xF], 0};
    print(s);
}

int main(void)
{
    int argc = (int)luggage_word_count();

    if (argc < 2)
    {
        println("Usage: show <filename>");
        exit(1);
        return 1;
    }

    file_id_t matches[16];
    int count = find_file_by_name(luggage_word(1), matches, NULL, 16);

    if (count < 0)
    {
        println("Error: Failed to query files");
        exit(1);
        return 1;
    }
    if (count == 0)
    {
        println("Error: File not found");
        exit(1);
        return 1;
    }

    if (count > 1)
    {
        println("Multiple files found:");
        int written = count < 16 ? count : 16;
        for (int i = 0; i < written; i++)
        {
            file_info_t info;
            if (file_info(matches[i], &info) == 0)
            {
                printf("  %s [file_id=%lu]\n", info.filename, matches[i]);
            }
        }
        println("Error: Ambiguous name (use file_id)");
        exit(1);
        return 1;
    }

    file_id_t file_id = matches[0];
    file_info_t info;
    if (file_info(file_id, &info) != 0)
    {
        println("Error: Failed to get file info");
        exit(1);
        return 1;
    }

    // printf("Content of %s (%lu bytes):\n", info.filename, (unsigned long)info.size);
    // println("----------------------------------------");

    /* Read and display file content in chunks. boxlib's fread no longer
     * has the old 168/176-byte Pocket payload cap (Phase 12 Manifest +
     * Crate makes it unbounded), so 4 KiB per fread is one storage
     * syscall instead of ~24. */
    char buffer[4096];
    size_t total_read = 0;
    size_t bytes_to_read = (info.size > 4096) ? 4096 : info.size; // limit display

    while (total_read < bytes_to_read)
    {
        size_t chunk = bytes_to_read - total_read;
        if (chunk > sizeof(buffer))
            chunk = sizeof(buffer);

        int result = fread(file_id, total_read, buffer, chunk);
        if (result < 0)
        {
            println("Error: Failed to read file content");
            exit(1);
            return 1;
        }
        if (result == 0)
            break;

        // Print the chunk (handle binary safely)
        for (int i = 0; i < result; i++)
            ShowByte(buffer[i]);

        total_read += result;
        if ((size_t)result < chunk)
            break;
    }

    // if (info.size > 4096) {
    //     println("\n----------------------------------------");
    //     printf("(Truncated: showing first 4096 of %lu bytes)\n", (unsigned long)info.size);
    // }

    println("");
    exit(0);
    return 0;
}
