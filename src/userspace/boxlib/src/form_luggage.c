/*
 * form_luggage.c — a filled Form as bytes in the Luggage, and back.
 *
 * The Luggage is the line as typed, a NUL, then records each ended by a NUL:
 * `filled 1`, a `field KIND` for every field of the Form in its order and
 * under it `#ID` for a file or `=TEXT` for a word, `screen #ID` for the
 * arrow, `souvenir` when the starter reads what the program gives back, and
 * `here` when a person stands at the screen the program is on.
 * The kernel hands these bytes over verbatim and any program may be
 * started with any bytes, so reading them is strict: a record that is not
 * exactly one of these, a field that does not match the program's own Form,
 * a count the kind does not allow, a required field with nothing in it — all
 * refuse the whole of it, and nothing is kept.
 */

#include "box/core/form_internal.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

/* ---- values ----------------------------------------------------------- */

void form_values_release(FormValues *values)
{
    for (uint32_t i = 0; i < values->FieldCount; i++) {
        FormFieldValue *field = &values->Fields[i];
        for (size_t w = 0; w < field->WordCount; w++) free(field->Words[w]);
        free(field->Words);
        free(field->Ids);
        free(field->Joined);
    }
    memset(values, 0, sizeof(*values));
}

int form_values_add_id(FormFieldValue *field, file_id_t id)
{
    file_id_t *bigger = realloc(field->Ids, (field->IdCount + 1) * sizeof(file_id_t));
    if (!bigger) return -ERR_NO_MEMORY;
    bigger[field->IdCount++] = id;
    field->Ids = bigger;
    return 0;
}

int form_values_add_word(FormFieldValue *field, const char *text, size_t length)
{
    char *copy = malloc(length + 1);
    if (!copy) return -ERR_NO_MEMORY;
    memcpy(copy, text, length);
    copy[length] = '\0';

    char **bigger = realloc(field->Words, (field->WordCount + 1) * sizeof(char *));
    if (!bigger) {
        free(copy);
        return -ERR_NO_MEMORY;
    }
    bigger[field->WordCount++] = copy;
    field->Words = bigger;
    return 0;
}

static bool takes_things(uint8_t kind)
{
    return kind == BOX_FORM_THING || kind == BOX_FORM_THINGS;
}

/* ---- writing ---------------------------------------------------------- */

typedef struct {
    char  *Bytes;
    size_t Used;
    size_t Capacity;
} FormBytes;

static int bytes_put(FormBytes *out, const char *text, size_t length)
{
    if (length == 0) return 0;
    if (out->Capacity - out->Used < length) {
        size_t wanted = out->Used + length;
        size_t grown  = out->Capacity ? out->Capacity * 2 : 256;
        while (grown < wanted) grown *= 2;
        char *bigger = realloc(out->Bytes, grown);
        if (!bigger) return -ERR_NO_MEMORY;
        out->Bytes    = bigger;
        out->Capacity = grown;
    }
    memcpy(out->Bytes + out->Used, text, length);
    out->Used += length;
    return 0;
}

static size_t decimal(file_id_t id, char *digits)
{
    char   reversed[20];
    size_t count = 0;

    do {
        reversed[count++] = (char)('0' + id % 10);
        id /= 10;
    } while (id != 0);
    for (size_t i = 0; i < count; i++) digits[i] = reversed[count - 1 - i];
    return count;
}

/* One record: a head, then the id in decimal or the text, then its NUL. */
static int put_record(FormBytes *out, const char *head, const char *text, size_t length,
                      bool is_id, file_id_t id)
{
    int rc = bytes_put(out, head, strlen(head));
    if (rc != 0) return rc;
    if (is_id) {
        char digits[20];
        rc = bytes_put(out, digits, decimal(id, digits));
    } else {
        rc = bytes_put(out, text, length);
    }
    return rc != 0 ? rc : bytes_put(out, "", 1);
}

static int put_field(FormBytes *out, const FormFieldValue *field)
{
    int rc = put_record(out, "field ", form_kind_name(field->Kind), strlen(form_kind_name(field->Kind)),
                        false, 0);
    for (size_t i = 0; rc == 0 && i < field->IdCount; i++)
        rc = put_record(out, "#", NULL, 0, true, field->Ids[i]);
    for (size_t i = 0; rc == 0 && i < field->WordCount; i++)
        rc = put_record(out, "=", field->Words[i], strlen(field->Words[i]), false, 0);
    return rc;
}

int form_records_encode(const FormValues *values, file_id_t screen, bool souvenir, bool here,
                        const char *line, size_t length, char **bytes, size_t *total)
{
    FormBytes out = { NULL, 0, 0 };

    int rc = bytes_put(&out, line, length);
    if (rc == 0) rc = bytes_put(&out, "", 1);
    if (rc == 0) rc = put_record(&out, "filled 1", NULL, 0, false, 0);
    for (uint32_t i = 0; rc == 0 && i < values->FieldCount; i++)
        rc = put_field(&out, &values->Fields[i]);
    if (rc == 0 && screen != 0) rc = put_record(&out, "screen #", NULL, 0, true, screen);
    if (rc == 0 && souvenir) rc = put_record(&out, "souvenir", NULL, 0, false, 0);
    if (rc == 0 && here) rc = put_record(&out, "here", NULL, 0, false, 0);
    if (rc != 0) {
        free(out.Bytes);
        return rc;
    }
    *bytes = out.Bytes;
    *total = out.Used;
    return 0;
}

/* ---- reading ---------------------------------------------------------- */

static bool starts_with(const char *text, size_t length, const char *head)
{
    size_t n = strlen(head);
    return length >= n && memcmp(text, head, n) == 0;
}

typedef struct {
    const BoxForm  *Form;
    FormValues     *Values;
    file_id_t       Screen;
    bool            Seen;
    bool            Souvenir;
    bool            Here;
} FormReader;

static int read_field(FormReader *reader, const char *name, size_t length)
{
    int kind = form_kind_of(name, length);
    FormValues *values = reader->Values;
    uint32_t    n = values->FieldCount;

    if (kind < 0 || !reader->Form || !reader->Form->Declared || n >= reader->Form->FieldCount ||
        reader->Form->Fields[n].Kind != kind)
        return -ERR_INVALID_ARGUMENT;
    values->Fields[n].Kind = (uint8_t)kind;
    values->FieldCount = n + 1;
    return 0;
}

static int read_item(FormReader *reader, const char *text, size_t length)
{
    FormValues *values = reader->Values;
    if (values->FieldCount == 0) return -ERR_INVALID_ARGUMENT;
    FormFieldValue *field = &values->Fields[values->FieldCount - 1];

    if (text[0] == '#') {
        file_id_t id;
        if (!takes_things(field->Kind) || !form_records_id(text + 1, length - 1, &id) ||
            (field->Kind == BOX_FORM_THING && field->IdCount >= 1))
            return -ERR_INVALID_ARGUMENT;
        return form_values_add_id(field, id);
    }
    if (takes_things(field->Kind) || length < 2 ||
        (field->Kind != BOX_FORM_WORDS && field->WordCount >= 1))
        return -ERR_INVALID_ARGUMENT;
    return form_values_add_word(field, text + 1, length - 1);
}

static int read_record(FormReader *reader, const char *text, size_t length)
{
    if (!reader->Seen) {
        reader->Seen = true;
        return length == 8 && memcmp(text, "filled 1", 8) == 0 ? 0 : -ERR_INVALID_ARGUMENT;
    }
    if (starts_with(text, length, "field ")) return read_field(reader, text + 6, length - 6);
    if (starts_with(text, length, "screen #")) {
        if (reader->Screen != 0 || !form_records_id(text + 8, length - 8, &reader->Screen))
            return -ERR_INVALID_ARGUMENT;
        return 0;
    }
    if (length == 8 && memcmp(text, "souvenir", 8) == 0) {
        if (reader->Souvenir) return -ERR_INVALID_ARGUMENT;
        reader->Souvenir = true;
        return 0;
    }
    if (length == 4 && memcmp(text, "here", 4) == 0) {
        if (reader->Here) return -ERR_INVALID_ARGUMENT;
        reader->Here = true;
        return 0;
    }
    if (length > 0 && (text[0] == '#' || text[0] == '=')) return read_item(reader, text, length);
    return -ERR_INVALID_ARGUMENT;
}

static bool fields_match_form(const BoxForm *form, const FormValues *values)
{
    uint32_t wanted = form && form->Declared ? form->FieldCount : 0;
    if (values->FieldCount != wanted) return false;
    for (uint32_t i = 0; i < wanted; i++) {
        const FormFieldValue *field = &values->Fields[i];
        if (!form->Fields[i].Optional && field->IdCount == 0 && field->WordCount == 0)
            return false;
    }
    return true;
}

int form_records_decode(const BoxForm *form, const char *bytes, size_t length,
                        FormValues *values, file_id_t *screen)
{
    memset(values, 0, sizeof(*values));
    *screen = 0;

    FormRecords records;
    if (!form_records_start(&records, bytes, length)) return 0;

    FormReader  reader = { form, values, 0, false, false, false };
    const char *text;
    size_t      size;
    int         rc = 0;

    while (rc == 0 && form_records_next(&records, &text, &size))
        rc = size == (size_t)-1 ? -ERR_INVALID_ARGUMENT : read_record(&reader, text, size);
    if (rc == 0 && (!reader.Seen || !fields_match_form(form, values))) rc = -ERR_INVALID_ARGUMENT;
    if (rc != 0) {
        form_values_release(values);
        return rc;
    }
    *screen = reader.Screen;
    return 1;
}
