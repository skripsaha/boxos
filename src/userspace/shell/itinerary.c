/*
 * itinerary.c — an Itinerary: steps of the shell kept in a file, run by name.
 *
 * A text file that wears `itinerary`. Its first lines are its Form — `takes`
 * and `gives`, as a program's — and every line after them is a step, run as
 * if it were typed. The person starts it as any program, by name: the words
 * are sorted into its Form, asking what must be asked, and the steps run one
 * after another.
 *
 *     takes things files "the files to tidy"
 *     takes words label "the tag they get"
 *     gives things
 *     tag files label
 *     show files
 *
 * A field's NAME is, in every step, what the field was given (box/form.h,
 * BoxFormGiven). `them` begins as the files of its first things field and
 * goes on as the steps give; when the Form gives things, `them` at the end is
 * what the Itinerary gave. The person's own `them` is neither read nor
 * changed inside it. A step that does not go through stops it: the rest is
 * not done. It has no conditions, so one that would run itself again —
 * directly or through another — would never end, and is not started.
 *
 * Steps print to the screen, one program after another: an arrow on the
 * Itinerary itself is for a Form that takes into, whose name the steps use.
 */

#include "itinerary.h"
#include "fill.h"
#include "shell.h"
#include "them.h"
#include "box/file.h"
#include "box/print.h"
#include "box/memory.h"
#include "box/string.h"
#include "box/error.h"

#define ITINERARY_PIECE 65536u

/* The Itineraries running, the innermost first. */
typedef struct ItineraryFrame {
    file_id_t              File;
    const BoxFormGiven    *Given;
    size_t                 GivenCount;
    struct ItineraryFrame *Outer;
} ItineraryFrame;

static ItineraryFrame *g_running;

typedef struct {
    char   *Text;              /* the whole file, each line ended by a NUL in place */
    char   *FormText;          /* `form 1` and the takes and gives lines */
    size_t  FormLength;
    char  **Steps;             /* into Text */
    size_t  StepCount;
} ItineraryText;

/* ---- the file ---------------------------------------------------------- */

/* Its bytes, whole; a short read is the volume answering with what it had in
 * hand, asked again. -ERR_INVALID_ARGUMENT: a NUL in it, so no text. */
static int ReadWhole(file_id_t file, char **out, size_t *length)
{
    FileMetadata metadata;
    int rc = FileMetadataRead(file, &metadata);
    if (rc != 0) return rc;
    uint64_t size = metadata.Size;
    FileMetadataRelease(&metadata);
    if (size > SIZE_MAX - 1) return -ERR_OUT_OF_RANGE;

    char *text = malloc((size_t)size + 1);
    if (!text) return -ERR_NO_MEMORY;
    for (uint64_t at = 0; at < size;) {
        uint64_t left = size - at;
        int64_t  got  = fread(file, at, text + at, left > ITINERARY_PIECE ? ITINERARY_PIECE : (size_t)left);
        if (got <= 0 || (uint64_t)got > left) {
            free(text);
            return got < 0 ? (int)got : -ERR_IO;
        }
        at += (uint64_t)got;
    }
    text[size] = '\0';
    if (memchr(text, '\0', (size_t)size)) {
        free(text);
        return -ERR_INVALID_ARGUMENT;
    }
    *out    = text;
    *length = (size_t)size;
    return 0;
}

static bool Blank(char c)
{
    return c == ' ' || c == '\t';
}

/* The line's first word is takes or gives: a line of the Form. */
static bool FormLine(const char *line)
{
    while (Blank(*line)) line++;
    size_t length = 0;
    while (line[length] && !Blank(line[length])) length++;
    return length == 5 && (memcmp(line, "takes", 5) == 0 || memcmp(line, "gives", 5) == 0);
}

static bool BlankLine(const char *line)
{
    while (Blank(*line)) line++;
    return *line == '\0';
}

static void Unload(ItineraryText *it)
{
    free(it->Text);
    free(it->FormText);
    free(it->Steps);
    memset(it, 0, sizeof(*it));
}

/* The lines cut where they end; the leading takes and gives lines (blank
 * lines among them passed over) are the Form, every line after them a step. */
static int Split(ItineraryText *it, size_t length)
{
    size_t lines = 1;
    for (size_t i = 0; i < length; i++) lines += it->Text[i] == '\n';

    static const char head[] = "form 1\n";
    it->Steps    = malloc(lines * sizeof(char *));
    it->FormText = malloc(sizeof(head) + length + 1);
    if (!it->Steps || !it->FormText) return -ERR_NO_MEMORY;
    memcpy(it->FormText, head, sizeof(head) - 1);
    it->FormLength = sizeof(head) - 1;

    bool  form = true;
    char *line = it->Text;
    while (line) {
        char *end  = strchr(line, '\n');
        char *next = end ? end + 1 : NULL;
        if (!end) end = line + strlen(line);
        *end = '\0';
        if (end > line && end[-1] == '\r') end[-1] = '\0';

        if (BlankLine(line)) {
            line = next;
            continue;
        }
        if (form && FormLine(line)) {
            size_t n = strlen(line);
            memcpy(it->FormText + it->FormLength, line, n);
            it->FormLength += n;
            it->FormText[it->FormLength++] = '\n';
        } else {
            form = false;
            it->Steps[it->StepCount++] = line;
        }
        line = next;
    }
    return 0;
}

/* The file read and cut into its Form and its steps; *form parsed. Says why
 * when it cannot be. */
static bool Load(file_id_t file, const char *who, ItineraryText *it, BoxForm *form)
{
    size_t length;
    memset(it, 0, sizeof(*it));
    int rc = ReadWhole(file, &it->Text, &length);
    if (rc == 0) rc = Split(it, length);
    if (rc == 0) rc = form_parse(it->FormText, it->FormLength, form);
    if (rc == 0) return true;

    if (rc == -ERR_INVALID_ARGUMENT && !it->Text)
        printf("%s: it is no text: there is a NUL in it\n", who);
    else if (rc == -ERR_INVALID_ARGUMENT)
        printf("%s: its takes and gives lines are no Form that can be filled\n", who);
    else
        printf("%s: it cannot be read (error %d)\n", who, (int)box_errno_of(rc));
    Unload(it);
    return false;
}

int ItineraryForm(file_id_t file, BoxForm *form)
{
    ItineraryText it;
    size_t        length;
    memset(&it, 0, sizeof(it));
    int rc = ReadWhole(file, &it.Text, &length);
    if (rc == 0) rc = Split(&it, length);
    if (rc == 0) rc = form_parse(it.FormText, it.FormLength, form);
    Unload(&it);
    return rc;
}

/* ---- the journey ------------------------------------------------------- */

void ItineraryShow(BoxFormLookup *lookup)
{
    lookup->Given      = g_running ? g_running->Given : NULL;
    lookup->GivenCount = g_running ? g_running->GivenCount : 0;
}

static bool TakesThings(uint8_t kind)
{
    return kind == BOX_FORM_THING || kind == BOX_FORM_THINGS;
}

/* `them` as the journey begins: the files of the first things field, or
 * nothing given when there is none. */
static bool BeginThem(const BoxForm *form, BoxFormFiller *filler)
{
    for (uint32_t i = 0; i < form->FieldCount; i++) {
        if (!TakesThings(form->Fields[i].Kind)) continue;

        const file_id_t   *ids;
        const char *const *words;
        size_t             id_count, word_count;
        if (form_fill_field(filler, i, &ids, &id_count, &words, &word_count) != 0) return false;
        file_id_t *copy = id_count ? malloc(id_count * sizeof(file_id_t)) : NULL;
        if (id_count && !copy) return false;
        if (id_count) memcpy(copy, ids, id_count * sizeof(file_id_t));
        ThemKeep(copy, id_count);
        return true;
    }
    return true;
}

/* What each named field was given; the arrays are the filler's. */
static bool GivenOf(const BoxForm *form, BoxFormFiller *filler, BoxFormGiven *given, size_t *count)
{
    *count = 0;
    for (uint32_t i = 0; i < form->FieldCount; i++) {
        if (!form->Fields[i].Name) continue;
        BoxFormGiven *one = &given[(*count)++];
        if (form_fill_field(filler, i, &one->Ids, &one->IdCount, &one->Words, &one->WordCount) != 0)
            return false;
        one->Name   = form->Fields[i].Name;
        one->Things = TakesThings(form->Fields[i].Kind);
    }
    return true;
}

/* The steps, one after another, until one does not go through or `exit` is
 * said. */
static bool Steps(const ItineraryText *it, const char *who)
{
    for (size_t i = 0; i < it->StepCount; i++) {
        if (ShellRunLine(it->Steps[i]) != 0) {
            printf("%s: step %u (%s) did not go through; the rest is not done\n", who,
                   (unsigned)(i + 1), it->Steps[i]);
            return false;
        }
        if (!ShellRunning()) break;
    }
    return true;
}

static bool Travel(const ItineraryText *it, const BoxForm *form, BoxFormFiller *filler,
                   file_id_t file, const char *who)
{
    BoxFormGiven given[BOX_FORM_FIELDS];
    size_t       given_count;
    ThemSaved    saved;

    ThemSave(&saved);
    if (!GivenOf(form, filler, given, &given_count) || !BeginThem(form, filler)) {
        ThemRestore(&saved);
        printf("%s: no memory to begin it\n", who);
        return false;
    }

    ItineraryFrame frame = { file, given, given_count, g_running };
    g_running = &frame;
    bool went = Steps(it, who);
    g_running = frame.Outer;

    if (went && form->Gives == BOX_FORM_GIVES_THINGS) ThemRelease(&saved);
    else ThemRestore(&saved);
    return went;
}

/* An arrow on the Itinerary itself goes into its into field; with none, its
 * steps' screens are many and there is no one screen to send. */
static bool ArrowFits(BoxFormFiller *filler, const char *who)
{
    file_id_t   screen;
    const char *create;
    if (form_fill_arrow(filler, &screen, &create) != 0) return false;
    if (screen == 0 && !create) return true;
    printf("%s: -> has no place here: it takes no into, and its steps print one after another\n", who);
    return false;
}

bool ItineraryRun(file_id_t file, const char *who, const char *line)
{
    for (const ItineraryFrame *frame = g_running; frame; frame = frame->Outer) {
        if (frame->File != file) continue;
        printf("%s: it would run itself again, and with nothing to stop it, it would never end; "
               "not started\n", who);
        return false;
    }

    ItineraryText it;
    BoxForm       form;
    if (!Load(file, who, &it, &form)) return false;

    BoxFormLookup  lookup;
    BoxFormFiller *filler;
    bool           went = false;
    FillLookup(&lookup);
    if (FillForm(&form, &lookup, who, line, &filler)) {
        if (ArrowFits(filler, who)) went = Travel(&it, &form, filler, file, who);
        form_fill_end(filler);
    }
    form_release(&form);
    Unload(&it);
    return went;
}
