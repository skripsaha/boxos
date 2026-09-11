#include "box/print.h"
#include "box/luggage.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

int main(void)
{
    int argc = (int)luggage_word_count();

    if (argc < 2)
    {
        println("Usage: show <filename>");
        exit(1);
        return 1;
    }

    uint32_t matches[16];
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
                printf("  %s [file_id=%u]\n", info.filename, matches[i]);
            }
        }
        println("Error: Ambiguous name (use file_id)");
        exit(1);
        return 1;
    }

    uint32_t file_id = matches[0];
    file_info_t info;
    if (file_info(file_id, &info) != 0)
    {
        println("Error: Failed to get file info");
        exit(1);
        return 1;
    }


    char buffer[4096];
    size_t total_read = 0;
    size_t bytes_to_read = (info.size > 4096) ? 4096 : info.size;

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

        for (int i = 0; i < result; i++)
        {
            char c = buffer[i];
            if (c >= 32 && c < 127)
            {
                char s[2] = {c, 0};
                print(s);
            }
            else if (c == '\n' || c == '\r' || c == '\t')
            {
                char s[2] = {c, 0};
                print(s);
            }
            else
            {
                printf("\\x%02x", (unsigned char)c);
            }
        }

        total_read += result;
        if ((size_t)result < chunk)
            break;
    }


    println("");
    exit(0);
    return 0;
}