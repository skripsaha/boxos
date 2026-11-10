/*
 * form_fill.c — the filler's calls: begin, the questions and their answers,
 * and the Luggage at the end.
 *
 * Which word goes to which field is settled in form_fill_assign.c, from the
 * start, after every answer. What lives here is the line cut into items, the
 * arrow taken out of it, and a state that says which answer is awaited.
 */

#include "box/form.h"
#include "box/core/form_filler.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

static char *copy_text(const char *text, size_t length)
{
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

int form_fill_refuse(BoxFormFiller *filler, const char *a, const char *b, const char *c)
{
    size_t la = strlen(a), lb = b ? strlen(b) : 0, lc = c ? strlen(c) : 0;
    char  *sentence = malloc(la + lb + lc + 1);
    if (!sentence) return -ERR_NO_MEMORY;

    memcpy(sentence, a, la);
    if (b) memcpy(sentence + la, b, lb);
    if (c) memcpy(sentence + la + lb, c, lc);
    sentence[la + lb + lc] = '\0';

    free(filler->Sentence);
    filler->Sentence = sentence;
    return BOX_FORM_REFUSED;
}

/* ---- the items -------------------------------------------------------- */

/* The running Itinerary's field a word names, plus 1; 0 for none. */
static uint32_t given_named(const BoxFormLookup *lookup, const char *word)
{
    for (size_t i = 0; lookup && i < lookup->GivenCount; i++) {
        if (strcmp(lookup->Given[i].Name, word) == 0) return (uint32_t)i + 1;
    }
    return 0;
}

/* What a word is, alone or in a comma list alike; quoted, it is a plain
 * word. A name of the running Itinerary's fields is what it was given —
 * files only where files are looked up, as `them`; the shell's words are
 * read only where files are looked up. */
static uint8_t role_of(const BoxFormLookup *lookup, const FormToken *token, bool keywords,
                       uint32_t *given)
{
    *given = 0;
    if (token->Quoted) return FORM_ROLE_WORD;
    uint32_t named = given_named(lookup, token->Text);
    if (named != 0 && (keywords || !lookup->Given[named - 1].Things)) {
        *given = named;
        return FORM_ROLE_GIVEN;
    }
    if (!keywords) return FORM_ROLE_WORD;
    if (strcmp(token->Text, "them") == 0) return FORM_ROLE_THEM;
    if (strcmp(token->Text, "it") == 0) return FORM_ROLE_IT;
    if (strcmp(token->Text, "all") == 0) return FORM_ROLE_ALL;
    return FORM_ROLE_WORD;
}

static int reserve_item(BoxFormFiller *filler)
{
    if (filler->ItemCount < filler->ItemCapacity) return 0;
    size_t    grown = filler->ItemCapacity ? filler->ItemCapacity * 2 : 8;
    FormItem *bigger = realloc(filler->Items, grown * sizeof(FormItem));
    if (!bigger) return -ERR_NO_MEMORY;
    filler->Items        = bigger;
    filler->ItemCapacity = grown;
    return 0;
}

int form_fill_add_items(BoxFormFiller *filler, FormTokens *tokens, uint8_t bound,
                        bool keywords, size_t skip_from, size_t skip_count)
{
    bool everywhere = false;

    for (size_t i = 0; i < tokens->Count; i++) {
        FormToken *token = &tokens->Tokens[i];

        if (skip_count != 0 && i == skip_from) filler->ArrowEverywhere = everywhere;
        if (i >= skip_from && i < skip_from + skip_count) continue;
        if (keywords && !token->Quoted && strcmp(token->Text, "everywhere") == 0) {
            everywhere = true;
            continue;
        }
        int rc = reserve_item(filler);
        if (rc != 0) return rc;

        FormItem *item = &filler->Items[filler->ItemCount++];
        memset(item, 0, sizeof(*item));
        item->Token      = *token;
        item->Role       = role_of(filler->Lookup, token, keywords, &item->Given);
        item->Everywhere = everywhere;
        item->Bound      = bound;
        token->Text      = NULL;
    }
    return 0;
}

/* `->` and the word after it, and a `(file)` after that. The tokens that make
 * the arrow are not items. */
static size_t find_arrow(BoxFormFiller *filler, const FormTokens *tokens, size_t *count)
{
    *count = 0;
    for (size_t i = 0; i < tokens->Count; i++) {
        const FormToken *token = &tokens->Tokens[i];
        if (token->Quoted || strcmp(token->Text, "->") != 0) continue;

        filler->ArrowStart = token->Start;
        filler->ArrowEnd   = token->End;
        *count = 1;
        if (i + 1 < tokens->Count) {
            filler->ArrowEnd = tokens->Tokens[i + 1].End;
            *count = 2;
            const FormToken *paren = i + 2 < tokens->Count ? &tokens->Tokens[i + 2] : NULL;
            if (paren && !paren->Quoted && paren->Text[0] == '(' &&
                paren->Text[strlen(paren->Text) - 1] == ')') {
                filler->ArrowEnd = paren->End;
                *count = 3;
            }
        }
        return i;
    }
    return 0;
}

static int take_arrow_name(BoxFormFiller *filler, const FormTokens *tokens, size_t at, size_t count)
{
    if (count < 2) {
        int rc = form_fill_refuse(filler, "-> wants the name of a file after it", NULL, NULL);
        if (rc != BOX_FORM_REFUSED) return rc;
        filler->Refused = 1;
        return 0;
    }
    const FormToken *target = &tokens->Tokens[at + 1];
    const char      *name = target->Text;
    uint32_t         named = target->Quoted ? 0 : given_named(filler->Lookup, name);

    /* The name of a field of the running Itinerary after the arrow is the
     * one word it was given. */
    if (named != 0) {
        const BoxFormGiven *given = &filler->Lookup->Given[named - 1];
        int rc = 0;
        if (given->Things || given->WordCount != 1) {
            rc = form_fill_refuse(filler, "-> wants a name after it, and ", name,
                                  given->Things ? " stands for files" : given->WordCount == 0
                                                                            ? " stands for no word"
                                                                            : " stands for more than one word");
            if (rc != BOX_FORM_REFUSED) return rc;
            filler->Refused = 1;
            return 0;
        }
        name = given->Words[0];
    }
    filler->ArrowName = copy_text(name, strlen(name));
    return filler->ArrowName ? 0 : -ERR_NO_MEMORY;
}

static bool form_has_thing(const BoxForm *form)
{
    for (uint32_t i = 0; i < form->FieldCount; i++) {
        if (form->Fields[i].Kind == BOX_FORM_THING || form->Fields[i].Kind == BOX_FORM_THINGS)
            return true;
    }
    return false;
}

static void items_release(BoxFormFiller *filler)
{
    for (size_t i = 0; i < filler->ItemCount; i++) {
        free(filler->Items[i].Token.Text);
        free(filler->Items[i].Ids);
    }
    free(filler->Items);
    filler->Items = NULL;
    filler->ItemCount = filler->ItemCapacity = 0;
}

void form_fill_end(BoxFormFiller *filler)
{
    if (!filler) return;
    items_release(filler);
    form_values_release(&filler->Values);
    free(filler->Line);
    free(filler->Program);
    free(filler->ArrowName);
    free(filler->ArrowIds);
    free(filler->Sentence);
    free(filler);
}

int form_fill_begin(const BoxForm *form, const char *line, BoxFormLookup *lookup,
                    BoxFormFiller **out)
{
    if (!form || !line || !out) return -ERR_INVALID_ARGUMENT;
    *out = NULL;

    BoxFormFiller *filler = calloc(1, sizeof(*filler));
    if (!filler) return -ERR_NO_MEMORY;
    filler->Form     = form;
    filler->Lookup   = lookup;
    filler->HasThing = form_has_thing(form);
    filler->Line     = copy_text(line, strlen(line));

    FormTokens tokens = { NULL, 0 };
    int rc = filler->Line ? 0 : -ERR_NO_MEMORY;
    if (rc == 0 && !lookup) rc = -ERR_INVALID_ARGUMENT;
    if (rc == 0) rc = form_line_cut(filler->Line, &tokens);
    if (rc == 0) {
        size_t count;
        size_t at = find_arrow(filler, &tokens, &count);
        if (count != 0) rc = take_arrow_name(filler, &tokens, at, count);
        if (rc == 0) rc = form_fill_add_items(filler, &tokens, 0, filler->HasThing, at, count);
    }
    form_line_release(&tokens);
    if (rc != 0) {
        form_fill_end(filler);
        return rc;
    }
    filler->Values.FieldCount = form->FieldCount;
    *out = filler;
    return 0;
}

/* ---- the steps and their answers -------------------------------------- */

int form_fill_step(BoxFormFiller *filler, BoxFormQuestion *question)
{
    if (!filler || !question) return -ERR_INVALID_ARGUMENT;
    memset(question, 0, sizeof(*question));

    if (!filler->Refused && !filler->Finished) {
        int step = form_fill_assign(filler);
        if (step < 0) return step;
        filler->Question.Step = (uint8_t)step;
        if (step == BOX_FORM_REFUSED) {
            filler->Refused = 1;
            filler->Question.Word = filler->Sentence;
        }
        if (step == BOX_FORM_DONE) filler->Finished = 1;
    }
    if (filler->Refused) {
        question->Step = BOX_FORM_REFUSED;
        question->Word = filler->Sentence;
    } else if (filler->Finished) {
        question->Step = BOX_FORM_DONE;
    } else {
        *question = filler->Question;
    }
    return 0;
}

int form_fill_choose(BoxFormFiller *filler, file_id_t choice)
{
    if (!filler) return -ERR_INVALID_ARGUMENT;
    bool arrow = filler->Pending == FORM_PENDING_ARROW;
    if (!arrow && filler->Pending != FORM_PENDING_WHICH) return -ERR_INVALID_ARGUMENT;

    file_id_t *ids   = arrow ? filler->ArrowIds : filler->Items[filler->PendingItem].Ids;
    size_t     count = arrow ? filler->ArrowCount : filler->Items[filler->PendingItem].IdCount;
    size_t     at    = 0;
    while (at < count && ids[at] != choice) at++;
    if (at == count) return -ERR_INVALID_ARGUMENT;

    if (arrow) {
        filler->ArrowChosen = choice;
    } else {
        ids[0] = choice;
        filler->Items[filler->PendingItem].IdCount = 1;
    }
    filler->Pending = FORM_PENDING_NONE;
    return 0;
}

int form_fill_sure(BoxFormFiller *filler, bool yes)
{
    if (!filler || filler->Pending != FORM_PENDING_SURE) return -ERR_INVALID_ARGUMENT;
    filler->Pending = FORM_PENDING_NONE;
    if (yes) {
        filler->Items[filler->PendingItem].Sure = 1;
        return 0;
    }
    int rc = form_fill_refuse(filler, "the whole volume was not agreed to", NULL, NULL);
    if (rc != BOX_FORM_REFUSED) return rc;
    filler->Refused = 1;
    return 0;
}

static bool takes_things_kind(uint8_t kind)
{
    return kind == BOX_FORM_THING || kind == BOX_FORM_THINGS;
}

/* An answer lets go of the last try at the field, and of the word of the line
 * that was named as the file that is not here. */
static void let_go_of_earlier(BoxFormFiller *filler, uint32_t field)
{
    for (size_t i = 0; i < filler->ItemCount; i++) {
        if (filler->Items[i].Bound == field + 1) filler->Items[i].Dropped = 1;
    }
    if (filler->Suspect != 0) filler->Items[filler->Suspect - 1].Dropped = 1;
    filler->Suspect = 0;
}

static int add_text_answer(BoxFormFiller *filler, uint32_t field, const char *words)
{
    while (*words == ' ' || *words == '\t') words++;
    size_t length = strlen(words);
    while (length > 0 && (words[length - 1] == ' ' || words[length - 1] == '\t')) length--;
    if (length == 0) return 0;

    int rc = reserve_item(filler);
    if (rc != 0) return rc;
    char *text = copy_text(words, length);
    if (!text) return -ERR_NO_MEMORY;

    FormItem *item = &filler->Items[filler->ItemCount++];
    memset(item, 0, sizeof(*item));
    item->Token.Text = text;
    item->Bound      = (uint8_t)(field + 1);
    return 0;
}

int form_fill_words(BoxFormFiller *filler, const char *words)
{
    if (!filler || !words || filler->Pending != FORM_PENDING_FIELD) return -ERR_INVALID_ARGUMENT;

    uint32_t field = filler->PendingField;
    uint8_t  kind  = filler->Form->Fields[field].Kind;
    filler->Reasons[field] = NULL;
    filler->Pending        = FORM_PENDING_NONE;
    let_go_of_earlier(filler, field);

    if (kind == BOX_FORM_TEXT) return add_text_answer(filler, field, words);

    FormTokens tokens;
    int rc = form_line_cut(words, &tokens);
    if (rc != 0) return rc;
    if ((kind == BOX_FORM_NAME || kind == BOX_FORM_INTO) && tokens.Count > 1) {
        filler->Reasons[field] = "one word is wanted";
        form_line_release(&tokens);
        return 0;
    }
    rc = form_fill_add_items(filler, &tokens, (uint8_t)(field + 1), takes_things_kind(kind) && filler->HasThing, 0, 0);
    form_line_release(&tokens);
    return rc;
}

int form_fill_field(BoxFormFiller *filler, uint32_t field, const file_id_t **ids, size_t *id_count,
                    const char *const **words, size_t *word_count)
{
    if (!filler || !filler->Finished || field >= filler->Values.FieldCount || !ids || !id_count ||
        !words || !word_count)
        return -ERR_INVALID_ARGUMENT;
    const FormFieldValue *value = &filler->Values.Fields[field];
    *ids        = value->Ids;
    *id_count   = value->IdCount;
    *words      = (const char *const *)value->Words;
    *word_count = value->WordCount;
    return 0;
}

int form_fill_program(BoxFormFiller *filler, const char *name)
{
    if (!filler || !name) return -ERR_INVALID_ARGUMENT;
    char *copy = copy_text(name, strlen(name));
    if (!copy) return -ERR_NO_MEMORY;
    free(filler->Program);
    filler->Program = copy;
    return 0;
}

int form_fill_souvenir(BoxFormFiller *filler)
{
    if (!filler) return -ERR_INVALID_ARGUMENT;
    filler->Souvenir = 1;
    return 0;
}

int form_fill_here(BoxFormFiller *filler)
{
    if (!filler) return -ERR_INVALID_ARGUMENT;
    filler->Here = 1;
    return 0;
}

int form_fill_arrow(BoxFormFiller *filler, file_id_t *screen, const char **create)
{
    if (!filler || !screen || !create || !filler->Finished) return -ERR_INVALID_ARGUMENT;
    *screen = filler->Screen;
    *create = filler->Create;
    return 0;
}

/* ---- the Luggage ------------------------------------------------------ */

static bool blank(char c)
{
    return c == ' ' || c == '\t';
}

/* The line from `from` to its end as typed, the arrow cut out of it (when it
 * lies in that part) and the blanks round the cut closed to one. */
char *form_fill_line_from(const BoxFormFiller *filler, size_t from, size_t *length)
{
    const char *line  = filler->Line;
    size_t      total = strlen(line);
    size_t      cut_from = total, cut_to = total;

    if (filler->ArrowName && filler->ArrowStart >= from) {
        cut_from = filler->ArrowStart;
        cut_to   = filler->ArrowEnd;
    }
    size_t head_end = cut_from;
    while (head_end > from && blank(line[head_end - 1])) head_end--;
    while (cut_to < total && blank(line[cut_to])) cut_to++;
    size_t tail_end = total;
    while (tail_end > cut_to && blank(line[tail_end - 1])) tail_end--;

    size_t head = head_end - from;
    size_t tail = tail_end - cut_to;
    char  *out  = malloc(head + tail + 2);
    if (!out) return NULL;

    memcpy(out, line + from, head);
    size_t at = head;
    if (head > 0 && tail > 0) out[at++] = ' ';
    memcpy(out + at, line + cut_to, tail);
    at += tail;
    out[at] = '\0';
    *length = at;
    return out;
}

const BoxFormGiven *form_fill_given(const BoxFormFiller *filler, const FormItem *item)
{
    return item->Role == FORM_ROLE_GIVEN ? &filler->Lookup->Given[item->Given - 1] : NULL;
}

typedef struct {
    char  *Bytes;
    size_t Used;
    size_t Capacity;
} FillText;

static int text_put(FillText *out, const char *text, size_t length)
{
    if (out->Capacity - out->Used <= length) {
        size_t grown = out->Capacity ? out->Capacity * 2 : 64;
        while (grown - out->Used <= length) grown *= 2;
        char *bigger = realloc(out->Bytes, grown);
        if (!bigger) return -ERR_NO_MEMORY;
        out->Bytes    = bigger;
        out->Capacity = grown;
    }
    memcpy(out->Bytes + out->Used, text, length);
    out->Used += length;
    out->Bytes[out->Used] = '\0';
    return 0;
}

/* The words given under a name, joined by single spaces. */
static int text_put_given(FillText *out, const BoxFormGiven *given)
{
    int rc = 0;
    for (size_t i = 0; rc == 0 && i < given->WordCount; i++) {
        if (i > 0) rc = text_put(out, " ", 1);
        if (rc == 0) rc = text_put(out, given->Words[i], strlen(given->Words[i]));
    }
    return rc;
}

/* The words of the text are cut again as the line was, so a word reads as the
 * same name here as it did as an item. */
char *form_fill_text_from(const BoxFormFiller *filler, size_t from, size_t *length)
{
    char *text = form_fill_line_from(filler, from, length);
    if (!text || filler->Lookup->GivenCount == 0) return text;

    FormTokens tokens;
    if (form_line_cut(text, &tokens) != 0) {
        free(text);
        return NULL;
    }
    FillText out = { NULL, 0, 0 };
    size_t   copied = 0;
    int      rc = text_put(&out, "", 0);
    for (size_t i = 0; rc == 0 && i < tokens.Count; i++) {
        const FormToken *token = &tokens.Tokens[i];
        uint32_t         named;
        if (role_of(filler->Lookup, token, false, &named) != FORM_ROLE_GIVEN) continue;
        rc = text_put(&out, text + copied, token->Start - copied);
        if (rc == 0) rc = text_put_given(&out, &filler->Lookup->Given[named - 1]);
        copied = token->End;
    }
    if (rc == 0) rc = text_put(&out, text + copied, *length - copied);
    form_line_release(&tokens);
    free(text);
    if (rc != 0) {
        free(out.Bytes);
        return NULL;
    }
    *length = out.Used;
    return out.Bytes;
}

int form_fill_luggage(BoxFormFiller *filler, file_id_t screen, char **bytes, size_t *length)
{
    if (!filler || !bytes || !length || !filler->Finished) return -ERR_INVALID_ARGUMENT;

    size_t words_length;
    char  *words = form_fill_line_from(filler, 0, &words_length);
    if (!words) return -ERR_NO_MEMORY;

    size_t name_length = filler->Program ? strlen(filler->Program) : 0;
    char  *line = malloc(name_length + words_length + 2);
    if (!line) {
        free(words);
        return -ERR_NO_MEMORY;
    }
    size_t at = 0;
    if (name_length) {
        memcpy(line, filler->Program, name_length);
        at = name_length;
        if (words_length) line[at++] = ' ';
    }
    memcpy(line + at, words, words_length);
    at += words_length;
    free(words);

    int rc = form_records_encode(&filler->Values, screen, filler->Souvenir, filler->Here, line, at, bytes,
                                 length);
    free(line);
    return rc;
}
