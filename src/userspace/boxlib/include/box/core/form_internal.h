#ifndef BOX_CORE_FORM_INTERNAL_H
#define BOX_CORE_FORM_INTERNAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/form.h"

/*
 * What the parts of Form share inside BoxLib: a filled Form as values, the
 * records that carry it in the Luggage, and the words that cut a line.
 */

typedef struct {
    uint8_t     Kind;          /* BoxFormKind */
    file_id_t  *Ids;
    size_t      IdCount;
    char      **Words;         /* each its own heap string */
    size_t      WordCount;
    char       *Joined;        /* the words joined by single spaces; the program side makes it */
} FormFieldValue;

typedef struct {
    FormFieldValue Fields[BOX_FORM_FIELDS];
    uint32_t       FieldCount;
} FormValues;

void form_values_release(FormValues *values);
int  form_values_add_id(FormFieldValue *field, file_id_t id);
int  form_values_add_word(FormFieldValue *field, const char *text, size_t length);

/* A KIND as the Form's text spells it, or NULL; and back (-1 when unknown). */
const char *form_kind_name(uint8_t kind);
int         form_kind_of(const char *name, size_t length);

/* The Luggage records for Values, after Line (Length bytes; the NUL after it
 * is written here). Screen 0 for none. */
int form_records_encode(const FormValues *values, file_id_t screen, const char *line,
                        size_t length, char **bytes, size_t *total);

/* The records of Luggage bytes read against Form (NULL: a program with no
 * Form). 1: decoded into Values and Screen. 0: there are no records — the
 * bytes are a line only. Negative: records that cannot be right, and nothing
 * is kept. */
int form_records_decode(const BoxForm *form, const char *bytes, size_t length,
                        FormValues *values, file_id_t *screen);

/* Only the `screen #ID` record, read without a Form; 0 for none or a bad one. */
file_id_t form_records_screen(const char *bytes, size_t length);

/* Walking the records of Luggage bytes. */
typedef struct {
    const char *At;
    const char *End;
} FormRecords;

/* False when the bytes have no records: no NUL, or nothing after it. */
bool form_records_start(FormRecords *records, const char *bytes, size_t length);
/* The next record's text and length; false at the end. A record with no NUL
 * to end it has length (size_t)-1. */
bool form_records_next(FormRecords *records, const char **text, size_t *length);
/* A canonical decimal id: digits only, no leading zero, not zero, 64 bits. */
bool form_records_id(const char *digits, size_t length, file_id_t *id);

/* The words of a line, cut as Luggage cuts them, with where each stood. */
typedef struct {
    char    *Text;             /* quotes removed, NUL-terminated, heap */
    uint32_t Start;            /* offset of the word as typed, opening quote included */
    uint32_t End;              /* offset after the word as typed, closing quote included */
    uint8_t  Quoted;
    uint8_t  CommaBefore;      /* a comma joins this word to the one before */
    uint8_t  CommaAfter;       /* a comma joins this word to the one after */
} FormToken;

typedef struct {
    FormToken *Tokens;
    size_t     Count;
} FormTokens;

int  form_line_cut(const char *line, FormTokens *out);
void form_line_release(FormTokens *tokens);

/* The program side: where the program's `screen #ID` file is, 0 for none.
 * Reads the Luggage only — it asks nobody and fills nothing. */
file_id_t form_screen_file(void);

#ifdef __cplusplus
}
#endif

#endif /* BOX_CORE_FORM_INTERNAL_H */
