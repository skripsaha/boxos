/*
 * form_records.c — the walk over the records in Luggage bytes, and the one
 * record every program's output depends on: `screen #ID`, where the arrow
 * points. It is kept apart from the rest of Form so that a program which never
 * reads a Form links only this, and print.c can ask where its output goes
 * without the filler coming with it.
 */

#include "box/core/form_internal.h"
#include "box/luggage.h"
#include "box/string.h"

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

file_id_t form_screen_file(void)
{
    static volatile uint32_t known;
    static file_id_t         screen;

    if (__atomic_load_n(&known, __ATOMIC_ACQUIRE)) return screen;

    Luggage l = luggage();
    screen = l.length ? form_records_screen(l.bytes, l.length) : 0;
    __atomic_store_n(&known, 1u, __ATOMIC_RELEASE);
    return screen;
}
