/*
 * form_fill_assign.c — the words of a line sorted into the fields of a Form.
 *
 * One sort runs from the first field to the last over the items the filler
 * holds, and ends with a step: a question, a refusal or done. A question
 * leaves behind what its answer will be read from (a chosen file on the item,
 * the words of an answer as items bound to their field), and the sort is run
 * again. Nothing is looked up twice: an item keeps what the lookup said.
 *
 * Every step of the sort returns 0 to go on, a BoxFormStep above 0 to stop
 * with it, or a negative error.
 *
 * thing and things take the words that are files, the order they were typed
 * in; name takes the next word that is no file, or failing that a file word
 * nothing else took; words takes all the words that are no file; text takes
 * the rest of the line as typed; into takes the arrow's target. A Form with no
 * thing field looks nothing up: every word is a plain one.
 *
 * The name of a field of the running Itinerary is what it was given: its
 * files are taken as `them` is, its words wherever words go.
 */

#include "box/core/form_filler.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/string.h"

#define NO_ITEM ((size_t)-1)

static bool usable(const FormItem *item, uint32_t field)
{
    return !item->Taken && !item->Dropped && (item->Bound == 0 || item->Bound == field + 1);
}

/* ---- looking up -------------------------------------------------------- */

static int refuse_lookup(BoxFormFiller *filler, const char *name)
{
    return form_fill_refuse(filler, "the volume would not say what ", name, " is");
}

static int resolve_ids(FormItem *item, const file_id_t *ids, size_t count)
{
    if (count == 0) return 0;
    item->Ids = malloc(count * sizeof(file_id_t));
    if (!item->Ids) return -ERR_NO_MEMORY;
    memcpy(item->Ids, ids, count * sizeof(file_id_t));
    item->IdCount = count;
    return 0;
}

/* The name of a field of the running Itinerary that was given words. */
static bool given_words(const BoxFormFiller *filler, const FormItem *item)
{
    const BoxFormGiven *given = form_fill_given(filler, item);
    return given && !given->Things;
}

static int resolve(BoxFormFiller *filler, FormItem *item)
{
    if (item->Resolved) return 0;
    BoxFormLookup *lookup = filler->Lookup;
    const BoxFormGiven *given = form_fill_given(filler, item);
    int rc = 0;

    if (item->Role == FORM_ROLE_THEM || item->Role == FORM_ROLE_IT) {
        if (lookup->ThemKnown) rc = resolve_ids(item, lookup->Them, lookup->ThemCount);
    } else if (given) {
        if (given->Things) rc = resolve_ids(item, given->Ids, given->IdCount);
    } else {
        file_id_t *ids = NULL;
        bool       bounded = false;
        int n = item->Role == FORM_ROLE_ALL
                    ? lookup->All(lookup, item->Everywhere != 0, &ids, &bounded)
                    : lookup->Find(lookup, item->Token.Text, item->Everywhere != 0, &ids);
        if (n < 0) {
            free(ids);
            return refuse_lookup(filler, item->Token.Text);
        }
        if (n == 0) free(ids);
        else item->Ids = ids;
        item->IdCount = (size_t)n;
        item->Bounded = bounded;
    }
    if (rc == 0) item->Resolved = 1;
    return rc;
}

/* Is the word one that names no file. Without a thing field nothing is
 * looked up and every word is plain. */
static int plain(BoxFormFiller *filler, FormItem *item, bool *is_plain)
{
    if (!filler->HasThing || given_words(filler, item)) {
        *is_plain = true;
        return 0;
    }
    int rc = resolve(filler, item);
    *is_plain = rc == 0 && item->Role == FORM_ROLE_WORD && item->IdCount == 0;
    return rc;
}

/* ---- questions -------------------------------------------------------- */

static int ask_which(BoxFormFiller *filler, size_t at)
{
    FormItem *item = &filler->Items[at];

    memset(&filler->Question, 0, sizeof(filler->Question));
    filler->Question.Word        = item->Token.Text;
    filler->Question.Choices     = item->Ids;
    filler->Question.ChoiceCount = item->IdCount;
    filler->Pending     = FORM_PENDING_WHICH;
    filler->PendingItem = at;
    return BOX_FORM_ASK_WHICH;
}

static int ask_sure(BoxFormFiller *filler, size_t at)
{
    memset(&filler->Question, 0, sizeof(filler->Question));
    filler->Question.Count = filler->Items[at].IdCount;
    filler->Pending     = FORM_PENDING_SURE;
    filler->PendingItem = at;
    return BOX_FORM_ASK_SURE;
}

static const char *failure_reason(BoxFormFiller *filler, const FormItem *item, int *error)
{
    *error = 0;
    if (item->Role == FORM_ROLE_THEM || item->Role == FORM_ROLE_IT)
        return filler->Lookup->ThemKnown ? "the last command gave nothing" : "nothing was given before";
    if (item->Role == FORM_ROLE_ALL) return "there are no files here";

    int rc = item->Role == FORM_ROLE_GIVEN
                 ? form_fill_refuse(filler, item->Token.Text, " was given no files", NULL)
                 : form_fill_refuse(filler, "there is no ", item->Token.Text, " here");
    if (rc != BOX_FORM_REFUSED) *error = rc;
    return filler->Sentence;
}

/* Why a required thing field is empty, when a word of the line says: `them`
 * with nothing behind it, or a name that looks like a file and is none. */
static const char *empty_reason(BoxFormFiller *filler, uint32_t field, int *error)
{
    *error = 0;
    for (size_t i = 0; i < filler->ItemCount; i++) {
        FormItem *item = &filler->Items[i];
        if (item->Dropped || !(item->Bound == 0 || item->Bound == field + 1)) continue;
        if (!item->Resolved || item->IdCount != 0 || given_words(filler, item)) continue;
        if (item->Role == FORM_ROLE_WORD && item->Bound == 0 && strchr(item->Token.Text, '.') == NULL)
            continue;

        if (item->Bound == 0) filler->Suspect = i + 1;
        return failure_reason(filler, item, error);
    }
    return NULL;
}

static int ask_field(BoxFormFiller *filler, uint32_t field)
{
    uint8_t     kind   = filler->Form->Fields[field].Kind;
    const char *reason = filler->Reasons[field];
    int         error  = 0;

    filler->Suspect = 0;
    if (!reason && (kind == BOX_FORM_THING || kind == BOX_FORM_THINGS))
        reason = empty_reason(filler, field, &error);
    if (error != 0) return error;

    memset(&filler->Question, 0, sizeof(filler->Question));
    filler->Question.Field = field;
    filler->Question.Word  = reason;
    filler->Pending      = FORM_PENDING_FIELD;
    filler->PendingField = field;
    return BOX_FORM_ASK_FIELD;
}

/* ---- thing and things ------------------------------------------------- */

/* Does the file word need an answer before it can be taken. `it` is one file:
 * several given ask which; `them` is all of them. */
static int admit(BoxFormFiller *filler, size_t at, bool single)
{
    FormItem *item = &filler->Items[at];

    if (item->Role == FORM_ROLE_ALL && !item->Bounded && !item->Sure) return ask_sure(filler, at);
    if (item->IdCount > 1 && (item->Role == FORM_ROLE_WORD || item->Role == FORM_ROLE_IT || single)) return ask_which(filler, at);
    return 0;
}

static bool linked(const FormItem *left, const FormItem *right)
{
    return left->Bound == right->Bound && (left->Token.CommaAfter || right->Token.CommaBefore);
}

/* A word of a comma list with a file in it that is itself no file. */
static bool list_holds_file(const BoxFormFiller *filler, size_t at)
{
    for (size_t i = at; i > 0 && linked(&filler->Items[i - 1], &filler->Items[i]); i--) {
        if (filler->Items[i - 1].Taken && filler->Items[i - 1].IdCount > 0) return true;
    }
    for (size_t i = at; i + 1 < filler->ItemCount && linked(&filler->Items[i], &filler->Items[i + 1]); i++) {
        if (filler->Items[i + 1].Taken && filler->Items[i + 1].IdCount > 0) return true;
    }
    return false;
}

static int check_lists(BoxFormFiller *filler, uint32_t field)
{
    for (size_t i = 0; i < filler->ItemCount; i++) {
        const FormItem *item = &filler->Items[i];
        if (!usable(item, field) || !item->Resolved || item->IdCount != 0 ||
            item->Role != FORM_ROLE_WORD || !list_holds_file(filler, i))
            continue;
        return form_fill_refuse(filler, "there is no ", item->Token.Text, " here");
    }
    return 0;
}

/* A field's files are files, each there once, however many words named it. */
static int add_file(FormFieldValue *out, file_id_t id)
{
    for (size_t i = 0; i < out->IdCount; i++) {
        if (out->Ids[i] == id) return 0;
    }
    return form_values_add_id(out, id);
}

static int take_things(BoxFormFiller *filler, uint32_t field)
{
    FormFieldValue *out = &filler->Values.Fields[field];
    bool   single = filler->Form->Fields[field].Kind == BOX_FORM_THING;
    size_t failed = NO_ITEM;

    for (size_t i = 0; i < filler->ItemCount; i++) {
        FormItem *item = &filler->Items[i];
        if (!usable(item, field) || given_words(filler, item)) continue;
        if (single && out->IdCount > 0) break;

        int rc = resolve(filler, item);
        if (rc != 0) return rc;
        if (item->IdCount == 0) {
            if (item->Role != FORM_ROLE_WORD) {
                item->Taken = 1;
                failed = i;
            }
            continue;
        }
        rc = admit(filler, i, single);
        if (rc != 0) return rc;
        for (size_t k = 0; k < item->IdCount; k++) {
            rc = add_file(out, item->Ids[k]);
            if (rc != 0) return rc;
        }
        item->Taken = 1;
    }
    if (out->IdCount == 0) return 0;
    if (failed != NO_ITEM) {
        int error;
        const char *reason = failure_reason(filler, &filler->Items[failed], &error);
        return error != 0 ? error : form_fill_refuse(filler, reason, NULL, NULL);
    }
    return check_lists(filler, field);
}

/* ---- name, words, text, into ------------------------------------------ */

/* The word an item is — or, for the name of words an Itinerary was given,
 * every one of those words. */
static int give_word(BoxFormFiller *filler, FormFieldValue *out, FormItem *item)
{
    const BoxFormGiven *given = form_fill_given(filler, item);
    int rc = 0;

    if (given && !given->Things) {
        for (size_t i = 0; rc == 0 && i < given->WordCount; i++)
            rc = form_values_add_word(out, given->Words[i], strlen(given->Words[i]));
    } else {
        rc = form_values_add_word(out, item->Token.Text, strlen(item->Token.Text));
    }
    if (rc == 0) item->Taken = 1;
    return rc;
}

static int take_name(BoxFormFiller *filler, uint32_t field, uint32_t *waiting)
{
    for (size_t i = 0; i < filler->ItemCount; i++) {
        FormItem *item = &filler->Items[i];
        if (!usable(item, field)) continue;

        bool is_plain;
        int  rc = plain(filler, item, &is_plain);
        if (rc != 0) return rc;
        if (!is_plain) continue;
        const BoxFormGiven *given = form_fill_given(filler, item);
        if (given && given->WordCount != 1)
            return form_fill_refuse(filler, item->Token.Text,
                                    given->WordCount == 0 ? " was given no word, and a name is one"
                                                          : " is more than one word, and a name is one",
                                    NULL);
        return give_word(filler, &filler->Values.Fields[field], item);
    }
    *waiting |= 1u << field;
    return 0;
}

static int take_words(BoxFormFiller *filler, uint32_t field)
{
    for (size_t i = 0; i < filler->ItemCount; i++) {
        FormItem *item = &filler->Items[i];
        if (!usable(item, field)) continue;

        bool is_plain;
        int  rc = plain(filler, item, &is_plain);
        if (rc == 0 && is_plain) rc = give_word(filler, &filler->Values.Fields[field], item);
        if (rc != 0) return rc;
    }
    return 0;
}

/* The names that found no plain word take a file word nothing took. */
static int take_waiting_names(BoxFormFiller *filler, uint32_t waiting)
{
    for (uint32_t field = 0; field < filler->Form->FieldCount; field++) {
        if (!(waiting & (1u << field))) continue;
        for (size_t i = 0; i < filler->ItemCount; i++) {
            FormItem *item = &filler->Items[i];
            if (!usable(item, field) || !item->Resolved || item->IdCount == 0 ||
                item->Role != FORM_ROLE_WORD)
                continue;
            int rc = give_word(filler, &filler->Values.Fields[field], item);
            if (rc != 0) return rc;
            break;
        }
    }
    return 0;
}

static int take_text(BoxFormFiller *filler, uint32_t field)
{
    FormFieldValue *out = &filler->Values.Fields[field];
    size_t first = NO_ITEM;

    for (size_t i = 0; i < filler->ItemCount; i++) {
        if (!usable(&filler->Items[i], field)) continue;
        if (filler->Items[i].Bound == field + 1) return give_word(filler, out, &filler->Items[i]);
        if (first == NO_ITEM) first = i;
    }
    if (first == NO_ITEM) return 0;

    size_t length;
    char  *rest = form_fill_text_from(filler, filler->Items[first].Token.Start, &length);
    if (!rest) return -ERR_NO_MEMORY;
    int rc = form_values_add_word(out, rest, length);
    free(rest);
    for (size_t i = 0; rc == 0 && i < filler->ItemCount; i++) {
        if (usable(&filler->Items[i], field)) filler->Items[i].Taken = 1;
    }
    return rc;
}

static int take_into(BoxFormFiller *filler, uint32_t field)
{
    FormFieldValue *out = &filler->Values.Fields[field];

    for (size_t i = 0; i < filler->ItemCount; i++) {
        FormItem *item = &filler->Items[i];
        if (item->Bound == field + 1 && usable(item, field)) return give_word(filler, out, item);
    }
    if (!filler->ArrowName || filler->ArrowToInto) return 0;
    filler->ArrowToInto = 1;
    return form_values_add_word(out, filler->ArrowName, strlen(filler->ArrowName));
}

static int take_field(BoxFormFiller *filler, uint32_t field, uint32_t *waiting)
{
    switch (filler->Form->Fields[field].Kind) {
    case BOX_FORM_THING:
    case BOX_FORM_THINGS: return take_things(filler, field);
    case BOX_FORM_NAME:   return take_name(filler, field, waiting);
    case BOX_FORM_WORDS:  return take_words(filler, field);
    case BOX_FORM_TEXT:   return take_text(filler, field);
    default:              return take_into(filler, field);
    }
}

/* ---- the arrow, what is left, what is missing -------------------------- */

static int settle_arrow(BoxFormFiller *filler)
{
    filler->Screen = 0;
    filler->Create = NULL;
    if (!filler->ArrowName || filler->ArrowToInto) return 0;

    if (!filler->ArrowResolved) {
        file_id_t *ids = NULL;
        int n = filler->Lookup->Find(filler->Lookup, filler->ArrowName, filler->ArrowEverywhere != 0, &ids);
        if (n < 0) {
            free(ids);
            return refuse_lookup(filler, filler->ArrowName);
        }
        if (n == 0) free(ids);
        else filler->ArrowIds = ids;
        filler->ArrowCount    = (size_t)n;
        filler->ArrowResolved = 1;
    }
    if (filler->ArrowCount == 0) {
        filler->Create = filler->ArrowName;
        return 0;
    }
    if (filler->ArrowCount == 1) {
        filler->Screen = filler->ArrowIds[0];
        return 0;
    }
    if (filler->ArrowChosen == 0) {
        memset(&filler->Question, 0, sizeof(filler->Question));
        filler->Question.Word        = filler->ArrowName;
        filler->Question.Choices     = filler->ArrowIds;
        filler->Question.ChoiceCount = filler->ArrowCount;
        filler->Pending = FORM_PENDING_ARROW;
        return BOX_FORM_ASK_WHICH;
    }
    filler->Screen = filler->ArrowChosen;
    return 0;
}

/* Whether a field that takes files was left without one. */
static bool things_missing(const BoxFormFiller *filler)
{
    for (uint32_t field = 0; field < filler->Form->FieldCount; field++) {
        uint8_t kind = filler->Form->Fields[field].Kind;
        if ((kind == BOX_FORM_THING || kind == BOX_FORM_THINGS) &&
            filler->Values.Fields[field].IdCount == 0)
            return true;
    }
    return false;
}

static int check_leftovers(BoxFormFiller *filler)
{
    if (!filler->Form->Declared) return 0;
    for (size_t i = 0; i < filler->ItemCount; i++) {
        const FormItem *item = &filler->Items[i];
        if (item->Bound != 0 || item->Taken || item->Dropped) continue;
        /* A word spelled like a file's name, where the command's file was left
         * out, is that file not being here — not a word out of place. */
        if (item->Role == FORM_ROLE_WORD && strchr(item->Token.Text, '.') && things_missing(filler))
            return form_fill_refuse(filler, "there is no ", item->Token.Text, " here");
        return form_fill_refuse(filler, item->Token.Text, " has no place in this command", NULL);
    }
    return 0;
}

static int check_required(BoxFormFiller *filler)
{
    for (uint32_t field = 0; field < filler->Form->FieldCount; field++) {
        const FormFieldValue *value = &filler->Values.Fields[field];
        if (filler->Form->Fields[field].Optional || value->IdCount != 0 || value->WordCount != 0)
            continue;
        return ask_field(filler, field);
    }
    return 0;
}

static void start_sort(BoxFormFiller *filler)
{
    for (size_t i = 0; i < filler->ItemCount; i++) filler->Items[i].Taken = 0;
    form_values_release(&filler->Values);
    filler->Values.FieldCount = filler->Form->FieldCount;
    for (uint32_t field = 0; field < filler->Form->FieldCount; field++)
        filler->Values.Fields[field].Kind = filler->Form->Fields[field].Kind;
    filler->ArrowToInto = 0;
    filler->Pending     = FORM_PENDING_NONE;
}

int form_fill_assign(BoxFormFiller *filler)
{
    uint32_t waiting = 0;
    int      step;

    start_sort(filler);
    for (uint32_t field = 0; field < filler->Form->FieldCount; field++) {
        step = take_field(filler, field, &waiting);
        if (step != 0) return step;
    }
    step = take_waiting_names(filler, waiting);
    if (step == 0) step = settle_arrow(filler);
    if (step == 0) step = check_leftovers(filler);
    if (step == 0) step = check_required(filler);
    return step;
}
