#include "box/print.h"
#include "box/ipc.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

/* Per-process scratch arrays. file_info_t ≈ 176B; an array of 256 occupies
 * ~45 KB which would be hostile to the 64 KB user stack — keep it in BSS. */
#define FILES_MAX 256
static uint32_t    s_file_ids[FILES_MAX];
static file_info_t s_file_infos[FILES_MAX];
static char        s_argv[16][64];
static char        s_query_tags[256];

int main(void)
{
    int argc;
    receive_args(&argc, s_argv, 16);

    s_query_tags[0] = '\0';

    if (argc > 1)
    {
        size_t pos = 0;
        for (int i = 1; i < argc && pos < 250; i++)
        {
            if (i > 1)
                s_query_tags[pos++] = ',';
            size_t len = strlen(s_argv[i]);
            if (pos + len < 250)
            {
                memcpy(s_query_tags + pos, s_argv[i], len);
                pos += len;
            }
        }
        s_query_tags[pos] = '\0';
    }

    int count = query((argc > 1) ? s_query_tags : NULL, s_file_ids, FILES_MAX);

    if (count < 0)
    {
        println("Error: Failed to query files");
        exit(1);
        return 1;
    }
    if (count > FILES_MAX)
    {
        println("Warning: Too many files, showing first 256");
        count = FILES_MAX;
    }
    if (count == 0)
    {
        println("No files found");
        exit(0);
        return 0;
    }

    println("Files:");

    for (int i = 0; i < count; i++)
    {
        if (file_info(s_file_ids[i], &s_file_infos[i]) != 0)
        {
            s_file_infos[i].filename[0] = '\0';
        }
    }

    /* Insertion sort by filename — O(n²) but stable, in-place, no extra
     * stack frame per swap (the previous bubble copied a 176-byte struct
     * twice per swap). For ≤256 entries this beats both bubble and qsort
     * given our memory budget. */
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
