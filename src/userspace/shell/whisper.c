/*
 * whisper.c — what the line at the prompt could go on to be.
 *
 * The editor lays it dim past the caret; Tab takes it a word at a time,
 * Shift+Tab all of it (box/print.h, LineWhisper). What is whispered, the first
 * that has anything:
 *
 *   - `them` — `it` when that is one file — on an empty word where the command
 *     still needs files and the last command gave some: what was just given
 *     is the likeliest thing to go on with;
 *   - the newest line typed in this Use Context that begins with the line, and
 *     then the newest typed in any;
 *   - a name the word being typed can be: a command's for the first word, and
 *     after it what the command takes — files where its Form takes things and
 *     after `->`, tags where it takes words. The Form is sorted by BoxLib's own
 *     filler over what the line can see, so a word is taken here as it will be
 *     at Enter, and only what the command still takes is offered — on an empty
 *     word, only what it still needs. Of several names, the one typed most
 *     lately wins, then the shortest. An empty word gets a name only when it
 *     is the one typed most lately, or the only one.
 *
 * Tab with nothing whispered says what the command still needs: the fields of
 * its Form the line has not filled, or what a built-in takes.
 */

#include "whisper.h"
#include "history.h"
#include "itinerary.h"
#include "program.h"
#include "shell.h"
#include "them.h"
#include "box/memory.h"
#include "box/string.h"

extern const ShellCommand g_commands[];

static char *Copy(const char *text, size_t n)
{
    char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, text, n);
    copy[n] = '\0';
    return copy;
}

static char *CopyText(const char *text)
{
    return Copy(text, strlen(text));
}

static bool IsBlank(char c)
{
    return c == ' ' || c == '\t';
}

/* The comma-joined `tags` hold the `length` bytes of `tag`. */
static bool HasTag(const char *tags, const char *tag, size_t length)
{
    while (tags && *tags) {
        const char *comma = strchr(tags, ',');
        size_t      n     = comma ? (size_t)(comma - tags) : strlen(tags);
        if (n == length && memcmp(tags, tag, length) == 0) return true;
        tags = comma ? comma + 1 : tags + n;
    }
    return false;
}

/* ---- the line as typed --------------------------------------------------- */

typedef struct {
    const char *Line;
    size_t      Length;
    size_t      Words;        /* whole words before the one typed; 0: it is the command's name */
    size_t      NameStart;    /* the command's name, once it is a whole word */
    size_t      NameEnd;
    size_t      WordStart;    /* the word being typed, its pieces before a comma and all */
    size_t      PieceStart;   /* its piece being typed: after a comma, or its opening quote */
    bool        Quoted;       /* that piece is inside "double quotes" */
    bool        AfterArrow;   /* the word before it is -> */
    bool        Everywhere;   /* `everywhere` stands before it */
} Typing;

static void WordDone(Typing *typing, size_t start, size_t end)
{
    const char *word   = typing->Line + start;
    size_t      length = end - start;
    if (typing->Words == 0) {
        typing->NameStart = start;
        typing->NameEnd   = end;
    }
    typing->AfterArrow = length == 2 && memcmp(word, "->", 2) == 0;
    if (length == 10 && memcmp(word, "everywhere", 10) == 0) typing->Everywhere = true;
    typing->Words++;
}

/* The line cut as the Luggage and the Form cut it: blanks between words,
 * "double quotes" round one, a comma between the pieces of a list. */
static void Read(Typing *typing, const char *line, size_t length)
{
    memset(typing, 0, sizeof(*typing));
    typing->Line   = line;
    typing->Length = length;

    bool   in_word = false, quoted = false;
    size_t word = 0, piece = 0;
    for (size_t i = 0; i < length; i++) {
        char c = line[i];
        if (quoted) {
            if (c == '"') quoted = false;
            continue;
        }
        if (IsBlank(c)) {
            if (in_word) WordDone(typing, word, i);
            in_word = false;
            continue;
        }
        if (!in_word) {
            in_word = true;
            word    = i;
            piece   = i;
        }
        if (c == '"') {
            quoted = true;
            piece  = i + 1;
        } else if (c == ',') {
            piece = i + 1;
        }
    }
    typing->Quoted     = quoted;
    typing->WordStart  = in_word ? word : length;
    typing->PieceStart = in_word ? piece : length;
}

/* The command's name: the first word, whole or being typed. */
static char *NameOf(const Typing *typing)
{
    if (typing->Words == 0)
        return Copy(typing->Line + typing->WordStart, typing->Length - typing->WordStart);
    return Copy(typing->Line + typing->NameStart, typing->NameEnd - typing->NameStart);
}

static const ShellCommand *BuiltIn(const char *name)
{
    for (int i = 0; g_commands[i].name != NULL; i++) {
        if (strcmp(g_commands[i].name, name) == 0) return &g_commands[i];
    }
    return NULL;
}

/* ---- the command's Form, sorted over what the line can see ---------------- */

typedef struct {
    BoxFormLookup Lookup;     /* first: what the filler is handed */
    ShellView    *View;
    bool          Bounded;
} Sight;

static int SightFind(BoxFormLookup *lookup, const char *name, bool everywhere, file_id_t **ids)
{
    return ViewFind(((Sight *)lookup)->View, name, everywhere, ids);
}

static int SightAll(BoxFormLookup *lookup, file_id_t **ids, bool *bounded)
{
    Sight *sight = (Sight *)lookup;
    *bounded = sight->Bounded;
    return ViewAll(sight->View, ids);
}

static void ReleaseForm(ShellWhisper *whisper)
{
    if (whisper->FormHeld) form_release(&whisper->Form);
    free(whisper->FormOf);
    whisper->FormOf   = NULL;
    whisper->FormHeld = false;
}

/* The Form of the program `name` means, read once for the line. */
static bool HoldForm(ShellWhisper *whisper, const char *name)
{
    if (whisper->FormOf && strcmp(whisper->FormOf, name) == 0) return whisper->FormHeld;
    ReleaseForm(whisper);
    whisper->FormOf = CopyText(name);
    file_id_t program = whisper->FormOf ? ViewProgram(&whisper->View, name) : 0;
    if (program == 0) return false;

    int rc = ProgramIsItinerary(program) ? ItineraryForm(program, &whisper->Form)
                                         : form_of_file(program, &whisper->Form);
    whisper->FormHeld = rc == 0;
    return whisper->FormHeld;
}

typedef enum {
    SORT_DONE,
    SORT_REFUSED,
    SORT_FAILED
} SortEnd;

/* `words` sorted into the held Form as Enter will sort them, over what the
 * line can see and asking nobody: every field made optional, so an empty one
 * stays empty rather than being asked for; a name that means several files
 * taken as the first, `all` taken as it is. filled[i]: field i got something.
 * On a refusal *refusal is its sentence, the caller's to free. */
static SortEnd SortForm(ShellWhisper *whisper, const char *words, bool *filled, char **refusal)
{
    BoxForm loose = whisper->Form;
    for (uint32_t i = 0; i < loose.FieldCount; i++) loose.Fields[i].Optional = 1;

    Sight sight;
    memset(&sight, 0, sizeof(sight));
    sight.Lookup.Find = SightFind;
    sight.Lookup.All  = SightAll;
    sight.View        = &whisper->View;
    sight.Bounded     = whisper->Context != NULL;
    ThemShow(&sight.Lookup);

    BoxFormFiller *filler = NULL;
    if (form_fill_begin(&loose, words, &sight.Lookup, &filler) != 0) {
        if (filler) form_fill_end(filler);
        return SORT_FAILED;
    }

    SortEnd end = SORT_FAILED;
    for (;;) {
        BoxFormQuestion question;
        if (form_fill_step(filler, &question) != 0) break;
        if (question.Step == BOX_FORM_DONE) {
            end = SORT_DONE;
            break;
        }
        if (question.Step == BOX_FORM_REFUSED) {
            if (refusal) *refusal = CopyText(question.Word);
            end = SORT_REFUSED;
            break;
        }
        int rc = question.Step == BOX_FORM_ASK_WHICH ? form_fill_choose(filler, question.Choices[0])
               : question.Step == BOX_FORM_ASK_SURE  ? form_fill_sure(filler, true)
                                                     : -1;
        if (rc != 0) break;
    }

    for (uint32_t i = 0; end == SORT_DONE && i < loose.FieldCount; i++) {
        const file_id_t   *ids;
        const char *const *given;
        size_t             id_count = 0, word_count = 0;
        filled[i] = form_fill_field(filler, i, &ids, &id_count, &given, &word_count) == 0 &&
                    id_count + word_count > 0;
    }
    form_fill_end(filler);
    return end;
}

/* ---- what the word being typed can be ------------------------------------- */

typedef struct {
    bool Commands;      /* a command's name */
    bool Programs;      /* a program's file */
    bool Files;         /* a file's name */
    bool Tags;          /* a tag */
    bool Everywhere;    /* the files and tags of the whole volume, not of the Use Context */
    bool Them;          /* `them` or `it` can stand here */
    bool Several;       /* and several files, not only one */
    bool Signed;        /* a + or - may come before it */
} Offer;

/* The field a Form kind fills with what can be whispered: files or tags. */
static void OfferKind(Offer *offer, uint8_t kind)
{
    if (kind == BOX_FORM_THING || kind == BOX_FORM_THINGS) offer->Files = true;
    if (kind == BOX_FORM_THINGS) offer->Several = true;
    if (kind == BOX_FORM_WORDS) offer->Tags = true;
}

/* What a program's Form takes now, the words before this one sorted. A word
 * begun may be anything the Form still takes; an empty one only what it still
 * needs — its first required field left empty — and there `them` is offered
 * when that field takes files. */
static void OfferProgram(ShellWhisper *whisper, const Typing *typing, const char *name, Offer *offer)
{
    if (!HoldForm(whisper, name) || !whisper->Form.Declared) return;
    char *before = Copy(typing->Line + typing->NameEnd, typing->WordStart - typing->NameEnd);
    if (!before) return;

    bool filled[BOX_FORM_FIELDS] = { false };
    SortEnd end = SortForm(whisper, before, filled, NULL);
    free(before);
    if (end != SORT_DONE) return;

    offer->Everywhere = typing->Everywhere;
    const BoxForm *form = &whisper->Form;
    if (typing->PieceStart == typing->Length) {
        for (uint32_t i = 0; i < form->FieldCount; i++) {
            if (form->Fields[i].Optional || filled[i]) continue;
            OfferKind(offer, form->Fields[i].Kind);
            offer->Them = offer->Files;
            return;
        }
        return;
    }
    for (uint32_t i = 0; i < form->FieldCount; i++) {
        if (form->Fields[i].Kind != BOX_FORM_THING || !filled[i]) OfferKind(offer, form->Fields[i].Kind);
    }
}

static void MakeOffer(ShellWhisper *whisper, const Typing *typing, Offer *offer)
{
    memset(offer, 0, sizeof(*offer));
    if (typing->Words == 0) {
        offer->Commands = true;
        return;
    }
    if (typing->AfterArrow) {
        offer->Files      = true;
        offer->Everywhere = typing->Everywhere;
        return;
    }

    char *name = NameOf(typing);
    if (!name) return;
    const ShellCommand *built_in = BuiltIn(name);
    if (!built_in) {
        OfferProgram(whisper, typing, name, offer);
    } else if (built_in->takes == SHELL_TAKES_COMMAND) {
        offer->Commands = typing->Words == 1;
    } else if (built_in->takes == SHELL_TAKES_PROGRAM) {
        offer->Programs = offer->Them = typing->Words == 1;
    } else if (built_in->takes == SHELL_TAKES_TAGS) {
        offer->Tags = offer->Everywhere = offer->Signed = true;
    }
    free(name);
}

/* ---- the names it can be -------------------------------------------------- */

/* The shelves the offer's names stand on; how many. */
static size_t OfferShelves(const Offer *offer, ViewShelfKind kinds[4])
{
    size_t count = 0;
    if (offer->Commands) kinds[count++] = VIEW_COMMANDS;
    if (offer->Programs) kinds[count++] = VIEW_PROGRAM_FILES;
    if (offer->Files) kinds[count++] = offer->Everywhere ? VIEW_FILES_EVERYWHERE : VIEW_FILES_HERE;
    if (offer->Tags) kinds[count++] = offer->Everywhere ? VIEW_TAGS_EVERYWHERE : VIEW_TAGS_HERE;
    return count;
}

/* The `length` bytes of `word` are one of the names the offer allows. */
static bool Named(ShellWhisper *whisper, const Offer *offer, const char *word, size_t length)
{
    ViewShelfKind kinds[4];
    for (size_t i = 0, n = OfferShelves(offer, kinds); i < n; i++) {
        if (ViewHolds(ViewShelfOf(&whisper->View, kinds[i]), word, length)) return true;
    }
    return false;
}

/* `name` can follow the `length` bytes of `typed` in a word: it begins with
 * them, goes on past them, and holds only what the word can — no blank, comma
 * or quote outside quotes, no quote inside. */
static bool Fits(const char *name, size_t name_len, const char *typed, size_t length, bool quoted)
{
    if (name_len <= length || memcmp(name, typed, length) != 0) return false;
    for (size_t i = 0; i < name_len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c >= 0x7F || c == '"') return false;
        if (!quoted && (c == ' ' || c == ',')) return false;
    }
    return true;
}

/* A name chosen: its bytes. */
typedef struct {
    const char *Name;
    size_t      Length;
} Choice;

/* ---- the words of the history, by when they were typed ------------------- */

/* A word as it was typed in a line of the history: Back the line, 0 the
 * newest. The word points into the history, which outlives the line. */
struct WhisperRecent {
    const char *Word;
    size_t      Length;
    size_t      Back;
};

/* The next word of a history line from *at — a piece of a comma list, its
 * quotes left off — or false at the end. */
static bool NextPiece(const char **at, const char **piece, size_t *length)
{
    const char *p = *at;
    while (*p && (IsBlank(*p) || *p == ',')) p++;
    if (!*p) return false;
    if (*p == '"') {
        const char *end = strchr(p + 1, '"');
        *piece  = p + 1;
        *length = end ? (size_t)(end - p - 1) : strlen(p + 1);
        *at     = end ? end + 1 : p + 1 + *length;
        return true;
    }
    const char *end = p;
    while (*end && !IsBlank(*end) && *end != ',') end++;
    *piece  = p;
    *length = (size_t)(end - p);
    *at     = end;
    return true;
}

/* Two words in byte order, a word before the words it begins; the same word
 * by the line it was typed in, the newest first. */
static int RecentOrder(const void *a, const void *b)
{
    const WhisperRecent *x = a, *y = b;
    size_t shorter = x->Length < y->Length ? x->Length : y->Length;
    int    order   = memcmp(x->Word, y->Word, shorter);
    if (order != 0) return order;
    if (x->Length != y->Length) return x->Length < y->Length ? -1 : 1;
    return x->Back < y->Back ? -1 : x->Back > y->Back;
}

/* Every word of the history, each once and with the newest line it was typed
 * in, in byte order: read once for the line, the first time it is wanted. */
static void ReadRecent(ShellWhisper *whisper)
{
    if (whisper->RecentRead) return;
    whisper->RecentRead = true;

    size_t      count = 0, room = 0;
    bool        full  = false;
    const char *line;
    for (size_t back = 0; !full && (line = HistoryLine(back, NULL)) != NULL; back++) {
        const char *at = line, *piece;
        size_t      length;
        while (!full && NextPiece(&at, &piece, &length)) {
            if (count == room) {
                size_t         grown = room ? room * 2 : 64;
                WhisperRecent *more  = realloc(whisper->Recent, grown * sizeof(WhisperRecent));
                full = !more;
                if (full) break;
                whisper->Recent = more;
                room            = grown;
            }
            whisper->Recent[count++] = (WhisperRecent){ piece, length, back };
        }
    }
    if (count == 0) return;

    whisper->RecentOrder = malloc(count * sizeof(WhisperRecent *));
    if (!whisper->RecentOrder) return;
    for (size_t i = 0; i < count; i++) whisper->RecentOrder[i] = &whisper->Recent[i];
    if (!ViewSort((const void **)whisper->RecentOrder, count, RecentOrder)) return;

    size_t kept = 0;
    for (size_t i = 0; i < count; i++) {
        const WhisperRecent *word = whisper->RecentOrder[i];
        const WhisperRecent *last = kept ? whisper->RecentOrder[kept - 1] : NULL;
        if (!last || last->Length != word->Length || memcmp(last->Word, word->Word, word->Length) != 0)
            whisper->RecentOrder[kept++] = word;
    }
    whisper->RecentCount = kept;
}

/* Where the words that begin with the `length` bytes of `typed` start. */
static size_t RecentFrom(const ShellWhisper *whisper, const char *typed, size_t length)
{
    size_t low = 0, high = whisper->RecentCount;
    while (low < high) {
        size_t               middle  = low + (high - low) / 2;
        const WhisperRecent *word    = whisper->RecentOrder[middle];
        size_t               shorter = word->Length < length ? word->Length : length;
        int                  order   = memcmp(word->Word, typed, shorter);
        bool                 before  = order < 0 || (order == 0 && word->Length < length);
        if (before) low = middle + 1;
        else        high = middle;
    }
    return low;
}

/* The name the word can become that was typed most lately. */
static bool Latest(ShellWhisper *whisper, const Offer *offer, const char *typed, size_t length,
                   bool quoted, Choice *choice)
{
    ReadRecent(whisper);
    const WhisperRecent *best = NULL;
    for (size_t i = RecentFrom(whisper, typed, length); i < whisper->RecentCount; i++) {
        const WhisperRecent *word = whisper->RecentOrder[i];
        if (word->Length < length || memcmp(word->Word, typed, length) != 0) break;
        if (best && word->Back >= best->Back) continue;
        if (Fits(word->Word, word->Length, typed, length, quoted) &&
            Named(whisper, offer, word->Word, word->Length))
            best = word;
    }
    if (!best) return false;
    choice->Name   = best->Word;
    choice->Length = best->Length;
    return true;
}

/* Of the names that begin with the word, the shortest, the first in order
 * among equals; *kinds how many differ, counted to two. */
static bool Shortest(ShellWhisper *whisper, const Offer *offer, const char *typed, size_t length,
                     bool quoted, Choice *choice, size_t *kinds)
{
    ViewShelfKind shelves[4];
    choice->Name = NULL;
    *kinds       = 0;
    for (size_t s = 0, n = OfferShelves(offer, shelves); s < n; s++) {
        const ViewShelf *shelf = ViewShelfOf(&whisper->View, shelves[s]);
        for (size_t i = ViewFrom(shelf, typed, length); i < shelf->Count; i++) {
            const char *name     = shelf->Names[i];
            size_t      name_len = strlen(name);
            if (strncmp(name, typed, length) != 0) break;
            if (!Fits(name, name_len, typed, length, quoted)) continue;

            bool same_size = choice->Name && choice->Length == name_len;
            int  order     = same_size ? memcmp(name, choice->Name, name_len) : 0;
            if (!same_size || order != 0) (*kinds)++;
            if (!choice->Name || name_len < choice->Length || (same_size && order < 0)) {
                choice->Name   = name;
                choice->Length = name_len;
            }
            /* An empty word is whispered only the one name there is: a second
             * ends the search. */
            if (length == 0 && *kinds > 1) return false;
        }
    }
    return choice->Name != NULL;
}

/* The name to whisper for the word being typed: the one typed most lately,
 * else the shortest, the first in order among equals. An empty word gets one
 * only from the history, or when it is the only name there is; a word that
 * already is a name gets none. */
static bool Choose(ShellWhisper *whisper, const Offer *offer, const char *typed, size_t length,
                   bool quoted, Choice *choice)
{
    if (length > 0 && Named(whisper, offer, typed, length)) return false;
    if (Latest(whisper, offer, typed, length, quoted, choice)) return true;

    size_t kinds;
    if (!Shortest(whisper, offer, typed, length, quoted, choice, &kinds)) return false;
    return length > 0 || kinds == 1;
}

/* ---- what is whispered ---------------------------------------------------- */

/* `them` — `it` when it is one file — on an empty word that can be files. */
static char *FromThem(const Offer *offer)
{
    bool   known;
    size_t count;
    (void)ThemFiles(&count, &known);
    if (!offer->Them || !known || count == 0) return NULL;
    return CopyText(count > 1 && offer->Several ? "them" : "it");
}

static bool SameTags(const char *a, const char *b)
{
    size_t count_a = 0, count_b = 0;
    for (const char *p = a; *p; p++) count_a += *p == ',';
    for (const char *p = b; *p; p++) count_b += *p == ',';
    if ((*a == '\0') != (*b == '\0') || count_a != count_b) return false;

    while (*a) {
        const char *comma  = strchr(a, ',');
        size_t      length = comma ? (size_t)(comma - a) : strlen(a);
        if (!HasTag(b, a, length)) return false;
        if (!comma) break;
        a = comma + 1;
    }
    return true;
}

/* The rest of the newest line of the history that begins with the line:
 * typed in this Use Context first, then in any. */
static char *FromHistory(ShellWhisper *whisper, const char *line, size_t length)
{
    const char *context_now = whisper->Context ? whisper->Context : "";
    for (int any = 0; any < 2; any++) {
        const char *entry, *context;
        for (size_t back = 0; (entry = HistoryLine(back, &context)) != NULL; back++) {
            if (!any && !SameTags(context_now, context)) continue;
            size_t entry_len = strlen(entry);
            if (entry_len > length && memcmp(entry, line, length) == 0)
                return Copy(entry + length, entry_len - length);
        }
    }
    return NULL;
}

static char *FromNames(ShellWhisper *whisper, const Typing *typing, const Offer *offer)
{
    const char *typed  = typing->Line + typing->PieceStart;
    size_t      length = typing->Length - typing->PieceStart;
    if (offer->Signed && length > 0 && (typed[0] == '+' || typed[0] == '-')) {
        typed++;
        length--;
    }
    Choice choice;
    if (!Choose(whisper, offer, typed, length, typing->Quoted, &choice)) return NULL;

    size_t rest = choice.Length - length;
    char  *said = malloc(rest + 2);
    if (!said) return NULL;
    memcpy(said, choice.Name + length, rest);
    if (typing->Quoted) said[rest++] = '"';
    said[rest] = '\0';
    return said;
}

static char *Continue(LineWhisper *line_whisper, const char *line, uint32_t length)
{
    ShellWhisper *whisper = (ShellWhisper *)line_whisper;
    Typing        typing;
    Offer         offer;
    bool          offered = false;
    Read(&typing, line, length);

    if (typing.Words > 0 && !typing.Quoted && typing.PieceStart == length) {
        MakeOffer(whisper, &typing, &offer);
        offered = true;
        char *them = FromThem(&offer);
        if (them) return them;
    }

    char *rest = FromHistory(whisper, line, length);
    if (rest) return rest;

    if (!offered) MakeOffer(whisper, &typing, &offer);
    return FromNames(whisper, &typing, &offer);
}

/* ---- what the command still needs ---------------------------------------- */

typedef struct {
    char  *Text;
    size_t Length;
    size_t Room;
} Words;

static void Say(Words *words, const char *text)
{
    size_t n = strlen(text);
    if (words->Length + n + 1 > words->Room) {
        size_t room = words->Room ? words->Room : 64;
        while (room < words->Length + n + 1) room *= 2;
        char *more = realloc(words->Text, room);
        if (!more) return;
        words->Text = more;
        words->Room = room;
    }
    memcpy(words->Text + words->Length, text, n + 1);
    words->Length += n;
}

/* "takes" and the fields the line has not filled: a required one as it is
 * said, an optional one in brackets, the arrow's before an `into`. */
static char *FormNeeds(ShellWhisper *whisper, const char *name, const char *words_typed)
{
    if (!HoldForm(whisper, name)) return NULL;
    const BoxForm *form = &whisper->Form;
    if (!form->Declared) return CopyText("takes its words as typed");
    if (form->FieldCount == 0) return CopyText("takes nothing");

    bool  filled[BOX_FORM_FIELDS] = { false };
    char *refusal = NULL;
    SortEnd end = SortForm(whisper, words_typed, filled, &refusal);
    if (end == SORT_REFUSED) return refusal;
    if (end != SORT_DONE) return NULL;

    Words said = { NULL, 0, 0 };
    for (uint32_t i = 0; i < form->FieldCount; i++) {
        const BoxFormField *field = &form->Fields[i];
        if (filled[i]) continue;
        Say(&said, said.Length ? ", " : "takes ");
        if (field->Optional) Say(&said, "[");
        if (field->Kind == BOX_FORM_INTO) Say(&said, "-> ");
        Say(&said, field->What);
        if (field->Optional) Say(&said, "]");
    }
    return said.Text;
}

/* What a built-in takes: its usage past its name. */
static char *BuiltInNeeds(const ShellCommand *built_in)
{
    const char *rest = strchr(built_in->usage, ' ');
    return rest ? CopyText(rest + 1) : NULL;
}

static char *Needs(LineWhisper *line_whisper, const char *line, uint32_t length)
{
    ShellWhisper *whisper = (ShellWhisper *)line_whisper;
    Typing        typing;
    Read(&typing, line, length);
    if (typing.Quoted || (typing.Words == 0 && typing.WordStart == length)) return NULL;

    char *name = NameOf(&typing);
    if (!name) return NULL;
    const ShellCommand *built_in = BuiltIn(name);
    const char         *words_typed = typing.Words == 0 ? "" : line + typing.NameEnd;
    char *needs = built_in ? BuiltInNeeds(built_in) : FormNeeds(whisper, name, words_typed);
    free(name);
    if (!needs || IsBlank(line[length - 1])) return needs;

    /* Laid straight after the last word, it would read as part of it. */
    size_t n      = strlen(needs);
    char  *spaced = malloc(n + 2);
    if (spaced) {
        spaced[0] = ' ';
        memcpy(spaced + 1, needs, n + 1);
    }
    free(needs);
    return spaced;
}

/* ---- the history ----------------------------------------------------------- */

static char *Recall(LineWhisper *line_whisper, uint32_t back)
{
    (void)line_whisper;
    const char *line = HistoryLine(back, NULL);
    return line ? CopyText(line) : NULL;
}

void WhisperBegin(ShellWhisper *whisper)
{
    memset(whisper, 0, sizeof(*whisper));
    whisper->Line.Continue = Continue;
    whisper->Line.Needs    = Needs;
    whisper->Line.Recall   = Recall;
    if (ShellContextWhole(&whisper->Context) <= 0) {
        free(whisper->Context);
        whisper->Context = NULL;
    }
    ViewBegin(&whisper->View, whisper->Context);
}

void WhisperEnd(ShellWhisper *whisper)
{
    ReleaseForm(whisper);
    ViewEnd(&whisper->View);
    free(whisper->RecentOrder);
    free(whisper->Recent);
    free(whisper->Context);
    memset(whisper, 0, sizeof(*whisper));
}
