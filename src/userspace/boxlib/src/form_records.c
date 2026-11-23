/*
 * form_records.c — the walk over the records in Luggage bytes, and the three
 * records a program reads without its Form: `screen #ID`, where the arrow
 * points, `souvenir`, the starter asking for what the program gives back, and
 * `here`, the starter saying a person stands at the program's screen.
 * They are kept apart from the rest of Form so that a program which never
 * reads a Form links only this: print.c asks where its output goes, and
 * souvenir_things() whether anyone wants its files, without the filler coming
 * with them.
 */

#include "box/core/form_internal.h"
#include "box/luggage.h"
#include "box/string.h"
#include "box/print.h"
#include "box/memory.h"

bool form_records_start(FormRecords *records, const char *bytes, size_t length)
{
    const char *nul = memchr(bytes, '\0', length);
    if (!nul || nul + 1 >= bytes + length) return false;
    records->At  = nul + 1;
    records->End = bytes + length;
    return true;
}

bool form_records_next(FormRecords *records, const char **text, size_t *length)
{
    if (records->At >= records->End) return false;

    const char *nul = memchr(records->At, '\0', (size_t)(records->End - records->At));
    *text = records->At;
    if (!nul) {
        *length = (size_t)-1;
        records->At = records->End;
        return true;
    }
    *length = (size_t)(nul - records->At);
    records->At = nul + 1;
    return true;
}

bool form_records_id(const char *digits, size_t length, file_id_t *id)
{
    if (length == 0 || length > 20 || digits[0] == '0') return false;

    file_id_t value = 0;
    for (size_t i = 0; i < length; i++) {
        if (digits[i] < '0' || digits[i] > '9') return false;
        file_id_t digit = (file_id_t)(digits[i] - '0');
        if (value > (UINT64_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *id = value;
    return true;
}

file_id_t form_records_screen(const char *bytes, size_t length)
{
    FormRecords records;
    if (!form_records_start(&records, bytes, length)) return 0;

    const char *text;
    size_t      size;
    file_id_t   screen = 0;

    while (form_records_next(&records, &text, &size)) {
        if (size == (size_t)-1) return 0;
        if (size < 8 || memcmp(text, "screen #", 8) != 0) continue;
        if (screen != 0 || !form_records_id(text + 8, size - 8, &screen)) return 0;
    }
    return screen;
}

/* Whether the record `word` stands once among the records: none, two, or
 * records that do not end, false. */
static bool form_records_once(const char *bytes, size_t length, const char *word)
{
    FormRecords records;
    if (!form_records_start(&records, bytes, length)) return false;

    const char  *text;
    size_t       size;
    const size_t wanted = strlen(word);
    bool         found  = false;

    while (form_records_next(&records, &text, &size)) {
        if (size == (size_t)-1) return false;
        if (size != wanted || memcmp(text, word, wanted) != 0) continue;
        if (found) return false;
        found = true;
    }
    return found;
}

bool form_records_souvenir(const char *bytes, size_t length)
{
    return form_records_once(bytes, length, "souvenir");
}

bool form_records_here(const char *bytes, size_t length)
{
    return form_records_once(bytes, length, "here");
}

bool form_souvenir_wanted(void)
{
    Luggage l = luggage_carried();
    return l.length != 0 && form_records_souvenir(l.bytes, l.length);
}

bool form_person_here(void)
{
    Luggage l = luggage_carried();
    return l.length != 0 && form_records_here(l.bytes, l.length);
}

bool form_yes(const char *question)
{
    if (!form_person_here()) return false;
    for (;;) {
        printf("%s (yes or no) ", question);
        char *typed = NULL;
        if (readline_grow(&typed) < 0) {
            println("");
            return false;
        }
        size_t from = 0, to = strlen(typed);
        while (from < to && typed[from] == ' ') from++;
        while (to > from && typed[to - 1] == ' ') to--;
        typed[to] = '\0';
        bool yes = strcmp(typed + from, "yes") == 0 || strcmp(typed + from, "y") == 0;
        bool no  = strcmp(typed + from, "no") == 0 || strcmp(typed + from, "n") == 0;
        free(typed);
        if (yes || no) return yes;
        printf("please answer yes or no. ");
    }
}

file_id_t form_screen_file(void)
{
    static volatile uint32_t known;
    static file_id_t         screen;

    if (__atomic_load_n(&known, __ATOMIC_ACQUIRE)) return screen;

    Luggage l = luggage_carried();
    screen = l.length ? form_records_screen(l.bytes, l.length) : 0;
    __atomic_store_n(&known, 1u, __ATOMIC_RELEASE);
    return screen;
}
