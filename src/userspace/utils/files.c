#include "box/print.h"
#include "box/luggage.h"
#include "box/memory.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

int main(void)
{
    int argc = (int)luggage_word_count();

    /* Every word after the name is a tag; joined with commas into a list
     * sized to what was typed, so no tag is ever dropped. */
    char *query_tags = NULL;
    if (argc > 1)
    {
        size_t total = 0;
        for (int i = 1; i < argc; i++) total += strlen(luggage_word(i)) + 1;
        query_tags = malloc(total);
        if (!query_tags)
        {
            println("Error: no memory for the tag list");
            exit(1);
            return 1;
        }
        size_t pos = 0;
        for (int i = 1; i < argc; i++)
        {
            size_t len = strlen(luggage_word(i));
            if (i > 1) query_tags[pos++] = ',';
            memcpy(query_tags + pos, luggage_word(i), len);
            pos += len;
        }
        query_tags[pos] = '\0';
    }

    /* Every file that answers, however many: the kernel says how many there
     * are and the list is sized to that. */
    uint32_t *s_file_ids = NULL;
    int count = query_all(query_tags, &s_file_ids);
    free(query_tags);

    if (count < 0)
    {
        println("Error: Failed to query files");
        exit(1);
        return 1;
    }
    if (count == 0)
    {
        println("No files found");
        exit(0);
        return 0;
    }

    file_info_t *s_file_infos = malloc((size_t)count * sizeof(file_info_t));
    if (!s_file_infos)
    {
        println("Error: no memory for the file list");
        exit(1);
        return 1;
    }

    println("Files:");

    for (int i = 0; i < count; i++)
    {
        if (file_info(s_file_ids[i], &s_file_infos[i]) != 0)
        {
            s_file_infos[i].filename[0] = '\0';
        }
    }

    /* Insertion sort by filename — stable and in place. */
    for (int i = 1; i < count; i++)
    {
        file_info_t key_info = s_file_infos[i];
        uint32_t    key_id   = s_file_ids[i];
        const char *key_name = key_info.filename;
        int j = i - 1;
        while (j >= 0 &&
               s_file_infos[j].filename[0] != '\0' &&
               key_name[0] != '\0' &&
               strcmp(s_file_infos[j].filename, key_name) > 0)
        {
            s_file_infos[j + 1] = s_file_infos[j];
            s_file_ids[j + 1]   = s_file_ids[j];
            j--;
        }
        s_file_infos[j + 1] = key_info;
        s_file_ids[j + 1]   = key_id;
    }

    for (int i = 0; i < count; i++)
    {
        if (s_file_ids[i] == 0 || s_file_infos[i].filename[0] == '\0')
            continue;

        const file_info_t *info = &s_file_infos[i];

        print(info->filename);

        size_t name_len = strlen(info->filename);
        for (size_t j = name_len; j < 32; j++)
            print(" ");

        print(" [");

        int printed_tags = 0;
        for (uint8_t t = 0; t < info->tag_count && t < 5; t++)
        {
            if (printed_tags > 0)
                print(", ");

            if (info->tags[t].value[0])
                printf("%s:%s", info->tags[t].key, info->tags[t].value);
            else
                print(info->tags[t].key);
            printed_tags++;
        }

        println("]");
    }

    exit(0);
    return 0;
}
