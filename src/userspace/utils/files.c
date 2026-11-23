#include "box/print.h"
#include "box/form.h"
#include "box/memory.h"
#include "box/file.h"
#include "box/string.h"
#include "box/system.h"
#include "box/error.h"

BOX_FORM("takes optional words \"the tags or names to look for, and on and a volume's name or number for one volume\"\n"
         "gives things\n");

/* Names are padded to the column the listing has always kept; a longer one
 * pushes its tags along. */
#define FILES_NAME_COLUMN 32u

/* Whether file `a` is listed before file `b`: by the volume it lies on, the
 * home volume first, then by name. */
static bool files_before(file_id_t a, const char *a_name, file_id_t b, const char *b_name)
{
    if (volume_of(a) != volume_of(b))
        return volume_of(a) < volume_of(b);
    return a_name && b_name && strcmp(a_name, b_name) < 0;
}

/* The heading of the files of volume `number`: the volume as a person calls
 * it (volume_shown), the home one said to be. */
static void files_heading(const VolumeListed *volumes, size_t count, uint32_t number)
{
    char shown[STORAGE_VOLUME_NAME_MOST + 64];
    const VolumeListed *v = NULL;
    for (size_t i = 0; i < count && !v; i++)
        if (volumes[i].Number == number)
            v = &volumes[i];
    if (!v)
        snprintf(shown, sizeof(shown), "volume %u", number);
    else
        volume_shown(v, shown, sizeof(shown));
    printf("On %s%s:\n", shown, v && v->Home ? " (home)" : "");
}

/* Whether the words end with `on` and a volume — its name, the words after
 * the last `on`, or its number (volume_named): the search narrowed to that
 * volume, the words taken off. 1 narrowed; 0 not, the words all a search's;
 * -1 for a name several volumes share or a number none has, said. */
static int files_on_volume(const char *const *words, size_t *word_count, uint32_t *volume)
{
    for (size_t on = *word_count; on-- > 0;) {
        if (strcmp(words[on], "on") != 0 || on + 1 == *word_count)
            continue;
        char   named[STORAGE_VOLUME_NAME_MOST + 1];
        size_t at = 0;
        for (size_t w = on + 1; w < *word_count; w++) {
            size_t len = strlen(words[w]);
            if (at + len + (at ? 1u : 0u) >= sizeof(named))
                return 0;
            if (at) named[at++] = ' ';
            memcpy(named + at, words[w], len);
            at += len;
        }
        named[at] = '\0';
        const char *rest   = named;
        int         called = volume_named(&rest, volume);
        if (called > 1) {
            printf("More than one volume is called %s — name it by its number (volumes shows them)\n", named);
            return -1;
        }
        if (called == 1 && !*rest) {
            *word_count = on;
            return 1;
        }
        if (called == 0 && strspn(named, "0123456789") == at) {
            printf("There is no volume numbered %s\n", named);
            return -1;
        }
        return 0;
    }
    return 0;
}

/* The volumes out of the machine, said after a listing: their files are
 * shown by nothing until each is back. */
static void files_away_said(void)
{
    char where[2u * STORAGE_VOLUME_NAME_MOST + 64u];
    int  away = volumes_away(NULL, 0, NULL, where, sizeof(where));
    if (away == 1)
        printf("%s is not plugged in: its files show again when it is\n", where);
    else if (away > 1)
        printf("%s are not plugged in: their files show again when they are\n", where);
}

/* The files `word` finds, on every volume or on `volume` alone (`narrowed`). */
static int files_word(const char *word, bool narrowed, uint32_t volume, file_id_t **out)
{
    return narrowed ? query_word_all_on(volume, PERSON_TAGS, word, out) : query_word_all(PERSON_TAGS, word, out);
}

/* The files every word finds — a word finds a file that wears it as a tag and
 * one that is called it (query_word_all) — as a list in file id order the
 * caller frees; no words find every file in the Use Context. On every volume
 * of the house, or on `volume` alone (`narrowed`). The count, or a negative
 * -error_t. */
static int files_found(const char *const *words, size_t word_count, bool narrowed, uint32_t volume, file_id_t **out)
{
    if (word_count == 0)
        return narrowed ? query_all_on(volume, PERSON_TAGS, NULL, NULL, out) : query_all(PERSON_TAGS, NULL, NULL, out);

    file_id_t *found = NULL;
    int        count = files_word(words[0], narrowed, volume, &found);
    for (size_t i = 1; count > 0 && i < word_count; i++)
    {
        file_id_t *next       = NULL;
        int        next_count = files_word(words[i], narrowed, volume, &next);
        if (next_count < 0)
        {
            free(found);
            return next_count;
        }
        int kept = 0;
        for (int a = 0, b = 0; a < count && b < next_count;)
        {
            if (found[a] < next[b])      a++;
            else if (next[b] < found[a]) b++;
            else                         { found[kept++] = found[a]; a++; b++; }
        }
        free(next);
        count = kept;
    }
    if (count <= 0)
    {
        free(found);
        *out = NULL;
        return count;
    }
    *out = found;
    return count;
}

int main(void)
{
    const char *const *words;
    size_t word_count;

    if (form_words(0, &words, &word_count) != 0)
    {
        exit(1);
        return 1;
    }

    /* Every file that answers, however many: the kernel says how many there
     * are and the list is sized to that. */
    uint32_t   volume = 0;
    int        on     = files_on_volume(words, &word_count, &volume);
    if (on < 0)
    {
        exit(1);
        return 1;
    }
    bool       narrowed   = on == 1;
    file_id_t *s_file_ids = NULL;
    int count = files_found(words, word_count, narrowed, volume, &s_file_ids);

    if (count == -ERR_DEVICE_NOT_READY)
    {
        if (narrowed)
        {
            char          shown[STORAGE_VOLUME_NAME_MOST + 64];
            VolumeListed *v = NULL;
            if (volume_info(volume, &v) == 0)
            {
                volume_shown(v, shown, sizeof(shown));
                free(v);
            }
            else
                snprintf(shown, sizeof(shown), "volume %u", volume);
            printf("%s is not plugged in: its files show again when it is\n", shown);
        }
        else
        {
            println("Nothing here answers that");
            files_away_said();
        }
        exit(1);
        return 1;
    }
    if (count < 0)
    {
        println("Error: Failed to query files");
        exit(1);
        return 1;
    }
    if (count == 0)
    {
        println("No files found");
        if (!narrowed)
            files_away_said();
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
        if (FileMetadataRead(PERSON_TAGS, s_file_ids[i], &s_metadata[i]) != 0)
            s_metadata[i].Filename = NULL;
    }

    /* Insertion sort by volume, then filename — stable and in place. */
    for (int i = 1; i < count; i++)
    {
        FileMetadata key_metadata = s_metadata[i];
        file_id_t    key_id       = s_file_ids[i];
        const char  *key_name     = key_metadata.Filename;
        int j = i - 1;
        while (j >= 0 && files_before(key_id, key_name, s_file_ids[j], s_metadata[j].Filename))
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
    /* Where the files lie is said once for each volume, when they lie on
     * more than one or on one that is not the home volume. */
    bool spread = narrowed;
    for (int i = 0; i < count; i++)
        spread = spread || volume_of(s_file_ids[i]) != 0;
    VolumeListed *volumes = NULL;
    size_t        volume_count = 0;
    if (spread && volume_list(&volumes, &volume_count) != 0)
        volume_count = 0;

    size_t   listed = 0;
    uint32_t said   = UINT32_MAX;
    for (int i = 0; i < count; i++)
    {
        const FileMetadata *metadata = &s_metadata[i];
        if (s_file_ids[i] == 0 || !metadata->Filename)
            continue;
        if (spread && volume_of(s_file_ids[i]) != said)
        {
            said = volume_of(s_file_ids[i]);
            files_heading(volumes, volume_count, said);
        }

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
    free(volumes);
    if (!narrowed)
        files_away_said();

    souvenir_things(s_file_ids, listed);

    exit(0);
    return 0;
}
