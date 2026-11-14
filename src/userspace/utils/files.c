#include "box/print.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"

BOX_FORM("takes optional words \"the tags to look for\"\n"
         "gives things\n");

/* Names are padded to the column the listing has always kept; a longer one
 * pushes its tags along. */
#define FILES_NAME_COLUMN 32u

int main(void)
{
    const char *const *words;
    size_t word_count;

    if (form_words(0, &words, &word_count) != 0)
    {
        exit(1);
        return 1;
    }

    /* Every word is a tag; joined with commas into a list sized to what was
     * typed, so no tag is ever dropped. */
    char *query_tags = NULL;
    if (word_count > 0)
    {
        size_t total = 0;
        for (size_t i = 0; i < word_count; i++) total += strlen(words[i]) + 1;
        query_tags = malloc(total);
        if (!query_tags)
        {
            println("Error: no memory for the tag list");
            exit(1);
            return 1;
        }
        size_t pos = 0;
        for (size_t i = 0; i < word_count; i++)
        {
            size_t len = strlen(words[i]);
            if (pos > 0) query_tags[pos++] = ',';
            memcpy(query_tags + pos, words[i], len);
            pos += len;
        }
        query_tags[pos] = '\0';
    }

    /* Every file that answers, however many: the kernel says how many there
     * are and the list is sized to that. */
    file_id_t *s_file_ids = NULL;
    int count = query_all(query_tags && *query_tags ? query_tags : NULL, NULL, &s_file_ids);
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

    /* Each file's whole metadata: its name and every tag at their own
     * length. file_info_t holds five tags, and a file found by its sixth
     * would be listed without the tag it was found by. */
    FileMetadata *s_metadata = calloc((size_t)count, sizeof(FileMetadata));
    if (!s_metadata)
    {
        println("Error: no memory for the file list");
        exit(1);
        return 1;
    }

    println("Files:");

    for (int i = 0; i < count; i++)
    {
        if (FileMetadataRead(s_file_ids[i], &s_metadata[i]) != 0)
            s_metadata[i].Filename = NULL;
    }

    /* Insertion sort by filename — stable and in place. */
    for (int i = 1; i < count; i++)
    {
        FileMetadata key_metadata = s_metadata[i];
        file_id_t    key_id       = s_file_ids[i];
        const char  *key_name     = key_metadata.Filename;
        int j = i - 1;
        while (j >= 0 && s_metadata[j].Filename && key_name &&
               strcmp(s_metadata[j].Filename, key_name) > 0)
        {
            s_metadata[j + 1] = s_metadata[j];
            s_file_ids[j + 1] = s_file_ids[j];
            j--;
        }
        s_metadata[j + 1] = key_metadata;
        s_file_ids[j + 1] = key_id;
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
     * The line is sized to the file: the name padded to the listing's column,
     * " [", the tags with ", " between them, and "]\n". */
    size_t listed = 0;
    for (int i = 0; i < count; i++)
    {
        const FileMetadata *metadata = &s_metadata[i];
        if (s_file_ids[i] == 0 || !metadata->Filename)
            continue;

        const char *tags = metadata->Tags ? metadata->Tags : "";
        size_t name_len = strlen(metadata->Filename);
        size_t tags_len = strlen(tags);
        char  *line = malloc(name_len + FILES_NAME_COLUMN + 2 * tags_len + 5);
        if (!line)
        {
            println("Error: no memory for a line of the list");
            break;
        }

        size_t pos = name_len;
        memcpy(line, metadata->Filename, name_len);
        while (pos < FILES_NAME_COLUMN)
            line[pos++] = ' ';
        line[pos++] = ' ';
        line[pos++] = '[';
        for (size_t t = 0; t < tags_len; t++)
        {
            line[pos++] = tags[t];
            if (tags[t] == ',' && tags[t + 1] != ' ')
                line[pos++] = ' ';
        }
        line[pos++] = ']';
        line[pos++] = '\n';

        print_bytes(line, pos);
        free(line);
        s_file_ids[listed++] = s_file_ids[i];
    }

    for (int i = 0; i < count; i++)
        FileMetadataRelease(&s_metadata[i]);
    free(s_metadata);

    souvenir_things(s_file_ids, listed);

    exit(0);
    return 0;
}
