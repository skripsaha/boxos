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
    file_id_t *s_file_ids = NULL;
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
        file_id_t   key_id   = s_file_ids[i];
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

    /* One file, one line, one call.
     *
     * The old loop asked print() once per padding blank, once per ", " and
     * once per tag name — about twenty-five calls a file, 1700 for this
     * volume. On a lane they merge into the frame and cost almost nothing,
     * which is why no board run ever showed it; with no display daemon — the
     * state the first shell starts in, and the one boxlib falls back to when
     * the daemon dies — every call is its own Manifest, its own console lock,
     * its own Canvas commit and its own blit, and the listing visibly crawls.
     * It is the same crawl boxlib's line editor was rewritten to remove.
     *
     * The buffer is the ABI's own arithmetic, and so is the tag column: a name
     * as wide as the field file_info_t hands back, " [", every tag slot as
     * key + ':' + value with ", " between them, and "]\n". Nothing here picks
     * a width of its own — the two 32s that used to sit in this loop were the
     * same 32 as box/file.h's name field, written out twice. */
    const size_t name_col  = sizeof(s_file_infos[0].filename);
    const size_t tag_slots = sizeof(s_file_infos[0].tags) /
                             sizeof(s_file_infos[0].tags[0]);
    char line[sizeof(s_file_infos[0].filename) + 2 +
              (sizeof(s_file_infos[0].tags) / sizeof(s_file_infos[0].tags[0])) *
              (sizeof(s_file_infos[0].tags[0].key) +
               sizeof(s_file_infos[0].tags[0].value) + 2) + 2];

    for (int i = 0; i < count; i++)
    {
        if (s_file_ids[i] == 0 || s_file_infos[i].filename[0] == '\0')
            continue;

        const file_info_t *info = &s_file_infos[i];

        size_t pos = strlen(info->filename);
        memcpy(line, info->filename, pos);
        while (pos < name_col)
            line[pos++] = ' ';

        line[pos++] = ' ';
        line[pos++] = '[';

        for (uint8_t t = 0; t < info->tag_count && t < tag_slots; t++)
        {
            if (t > 0)
            {
                line[pos++] = ',';
                line[pos++] = ' ';
            }

            size_t key_len = strlen(info->tags[t].key);
            memcpy(line + pos, info->tags[t].key, key_len);
            pos += key_len;

            if (info->tags[t].value[0])
            {
                size_t value_len = strlen(info->tags[t].value);
                line[pos++] = ':';
                memcpy(line + pos, info->tags[t].value, value_len);
                pos += value_len;
            }
        }

        line[pos++] = ']';
        line[pos++] = '\n';

        print_bytes(line, pos);
    }

    exit(0);
    return 0;
}
