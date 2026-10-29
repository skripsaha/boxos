#ifndef BOX_FORM_H
#define BOX_FORM_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"

/*
 * Form — what a program says it takes and what it gives.
 *
 * A program declares its Form once, in its image:
 *
 *     BOX_FORM("takes things \"the files to tag\"\n"
 *              "takes words \"the tags they get\"\n"
 *              "gives things\n");
 *
 * (In BoxASM: the directives `takes things "the files to tag"` and
 * `gives things`.) The shell reads it before the program starts and sorts the
 * words the person typed by what each one IS — a file that is here, `them`
 * or `it` (what the last command gave), `all` (everything in the Use
 * Context), or a plain word — into the fields, in their order. It asks only
 * when it must: a name that means more than one file, a field left empty,
 * `all` with no context (the whole volume). The program reads its fields
 * filled — file ids, words, text — and never parses its own line.
 *
 *     tag a.txt done            things [a.txt]          words [done]
 *     tag done a.txt, b.txt     things [a.txt, b.txt]   words [done]
 *     tag them done             things [what files gave] words [done]
 *     rename note.txt idea.txt  thing [note.txt]        name [idea.txt]
 *
 * ---- The Form's text (section .boxos.form of the image) -----------------
 *
 * Lines, each ended by '\n':
 *
 *     form 1                                 first; the toolchain writes it
 *     takes [optional] KIND [NAME] "what it is"   a field, in order
 *     gives things | text                    what the program gives back
 *
 * KIND:
 *     thing    one file that is here
 *     things   one file or more: names, them, it, all — a list with commas of any of them
 *     name     one word, a name new or not: the program makes of it what it is
 *     words    words, tags mostly, as many as are typed
 *     text     the rest of the line as typed, sorted into nothing
 *     into     the name `->` points at: the program puts its result there
 *
 * NAME, when a field has one, is what an Itinerary's steps call what it was
 * given (see BoxFormGiven): a letter, then letters, digits and '_'; not
 * them, it, all or everywhere; one field's only. A program may name its
 * fields too, and `help` shows the names.
 *
 * A field is required — at least one item — unless it says `optional`.
 * "what it is" is a few words the shell says when it asks for the field and
 * `help` shows: "the files to tag". A line the reader does not know is
 * passed over: a later Form may say more, an older reader reads what it
 * knows. A Form holds at most BOX_FORM_FIELDS fields.
 *
 * ---- The filled Form, in the Luggage -----------------------------------
 *
 * The shell starts the program by its file id with the Luggage: the line as
 * typed, a NUL, then NUL-ended records —
 *
 *     filled 1          first
 *     field KIND        a field begins, in the Form's order, KIND as above
 *     #ID               a file of the field, its id in decimal
 *     =TEXT             a word of the field (a name and the text are one each)
 *     screen #ID        what the program prints is added to the end of this file
 *     souvenir          the starter reads what the program gives back
 *
 * ---- The arrow and what a command gives --------------------------------
 *
 * `->` and the word after it put the result into a thing. A program with an
 * `into` field is handed the name and puts its result there itself; what it
 * made, it gives back with souvenir_things(), and that is what `them` is
 * after it. Any other program writes to the screen, and the arrow turns the
 * screen into the file (`screen #ID`): then that file is what the command
 * gave — `planets -> out.txt` makes `them` and `it` the file out.txt. The
 * shell does this once the program is gone.
 *
 * A Souvenir goes only to a starter that asked for it (`souvenir`). The
 * shell asks when it will keep it: the Form gives things and the arrow did
 * not make the screen a file. A program started by anyone else gives back
 * to no one.
 *
 * `them` is all that the last command gave; `it` is one file: the file when
 * it gave one, a question which when it gave several. A program that fills its
 * own Form has no one to ask and refuses `it` with several, as any question.
 *
 * A program without a Form still gets `filled 1` and, with an arrow,
 * `screen #ID`, and its line without the arrow. luggage_word() and
 * luggage_tail() read the line only, as they always did.
 */

#define BOX_FORM_FIELDS 16u

typedef enum {
    BOX_FORM_THING = 0,
    BOX_FORM_THINGS,
    BOX_FORM_NAME,
    BOX_FORM_WORDS,
    BOX_FORM_TEXT,
    BOX_FORM_INTO
} BoxFormKind;

typedef enum {
    BOX_FORM_GIVES_NOTHING = 0,
    BOX_FORM_GIVES_THINGS,
    BOX_FORM_GIVES_TEXT
} BoxFormGives;

/* BOX_FORM(text): the program's Form, text as above without `form 1`. One
 * per program. It lands in .boxos.form, read-only, beside .nameplate. */
#define BOX_FORM(Text)                                                         \
    __attribute__((section(".boxos.form"), used, aligned(1)))                  \
    static const char BoxFormDeclared[] = "form 1\n" Text

/* ---- What a program reads --------------------------------------------- */

/* Field N of the program's Form, 0 first, filled: by the shell when it
 * started the program, or else — started by another program, by autostart —
 * by BoxLib itself from the line at the first ask, by the same rule and
 * asking nobody. A refusal (a field that cannot be filled, a name that means
 * more than one file, no Form) is said on the screen once, and every ask
 * answers -ERR_INVALID_ARGUMENT; the program ends.
 *
 * form_things: the field's files (thing, things). form_words: its words
 * (words, name, text, into: a name or the text is one word). form_text:
 * the field as one text, its words joined by single spaces; NULL on a
 * refusal. The arrays live as long as the program. */
int         form_things(uint32_t field, const file_id_t **ids, size_t *count);
int         form_words(uint32_t field, const char *const **words, size_t *count);
const char *form_text(uint32_t field);

/* Give files back to the one who started the program, when it asked for
 * them — the shell keeps them as `them` for the next command. Called again,
 * the files add up. 0, or a negative -error_t (nobody asked, or the one who
 * did is gone: 0, given to no one). */
int souvenir_things(const file_id_t *ids, size_t count);

/* The shell's side: every file the program `from_pid` gave back, in the order
 * given, into a heap array the caller frees (NULL when none). Reads what is
 * pending without waiting; messages that are not souvenirs from that pid are
 * left in the queue as they were. Call it once the program is gone: what a
 * program gave is queued before its end is seen. 0, or a negative -error_t
 * (nothing is taken from the queue then). */
int souvenir_collect(uint32_t from_pid, file_id_t **ids, size_t *count);

/* ---- What the shell uses ----------------------------------------------- */

typedef struct {
    uint8_t Kind;              /* BoxFormKind */
    uint8_t Optional;
    char   *What;              /* "the files to tag" */
    char   *Name;              /* the field's NAME, NULL when it has none */
} BoxFormField;

typedef struct {
    BoxFormField Fields[BOX_FORM_FIELDS];
    uint32_t     FieldCount;
    uint8_t      Gives;        /* BoxFormGives */
    uint8_t      Declared;     /* the program has a Form */
} BoxForm;

/* The Form from its text; text not starting with `form 1` is no Form. */
int  form_parse(const char *text, size_t length, BoxForm *out);
/* The Form of a program on the volume, read from its file's .boxos.form
 * section (the headers and that section only). A program with none gets
 * Declared 0 and 0. */
int  form_of_file(file_id_t program, BoxForm *out);
void form_release(BoxForm *form);

/* What an Itinerary was given under one of its fields' names. A step's word
 * that is such a name — not quoted; alone or in a comma list — is what was given:
 * files where files are taken (as `them` is; to a Form with no thing field
 * the name is a plain word), words wherever words are taken, the words
 * joined by single spaces in a text field, and the one word of a name or of
 * an arrow's target. */
typedef struct {
    const char        *Name;
    uint8_t            Things;     /* the field took files: Ids; else Words */
    const file_id_t   *Ids;
    size_t             IdCount;
    const char *const *Words;
    size_t             WordCount;
} BoxFormGiven;

/* How the filler sees the volume: the shell's lookups. Each answers a count
 * and an array the caller frees, or a negative -error_t. */
typedef struct BoxFormLookup BoxFormLookup;
struct BoxFormLookup {
    /* Every file called Name in the Use Context, or on the whole volume
     * when Everywhere. */
    int (*Find)(BoxFormLookup *lookup, const char *name, bool everywhere, file_id_t **ids);
    /* Every file in the Use Context (`all`); *Bounded 0 when there is no
     * context and the answer is the whole volume. */
    int (*All)(BoxFormLookup *lookup, file_id_t **ids, bool *bounded);
    const file_id_t *Them;     /* `them` all of it, `it` one of it: what the last command gave */
    size_t           ThemCount;
    bool             ThemKnown;
    const BoxFormGiven *Given; /* the running Itinerary's fields, by name; none outside one */
    size_t              GivenCount;
};

/* The volume as BoxLib sees it — names by their stem tag, held to the whole
 * name, in the Use Context or everywhere; `all` the Use Context — the same
 * lookups a program filling its own Form uses. Them is left empty: the
 * shell sets it. */
void form_lookup_volume(BoxFormLookup *lookup);

typedef enum {
    BOX_FORM_DONE = 0,
    BOX_FORM_ASK_WHICH,        /* Word (a name, or `it`) means several files: choose one of Choices */
    BOX_FORM_ASK_FIELD,        /* Field is required and empty: words for it */
    BOX_FORM_ASK_SURE,         /* `all` is the whole volume, Count files: yes or no */
    BOX_FORM_REFUSED           /* the line cannot be filled: Word is the sentence */
} BoxFormStep;

typedef struct {
    uint8_t          Step;     /* BoxFormStep */
    uint32_t         Field;    /* ASK_FIELD */
    const char      *Word;     /* ASK_WHICH: the name; ASK_FIELD: why it is empty, or NULL;
                                  REFUSED: what is wrong, a sentence — every Word lives until
                                  the next call on the filler */
    const file_id_t *Choices;  /* ASK_WHICH */
    size_t           ChoiceCount;
    size_t           Count;    /* ASK_SURE */
} BoxFormQuestion;

typedef struct BoxFormFiller BoxFormFiller;

/* Begin filling Form from Line — the words after the program's name, as
 * typed. The words are cut as Luggage cuts them (blanks, "double quotes"); a
 * comma ends a word and joins the next to its list; `->` and the word after
 * it are the arrow; `them` `it` `all` `everywhere` are the shell's words —
 * alone or in a list alike, plain when quoted — but only for a Form with a
 * thing or things field — to any other they are plain words. A file is in a
 * field once, however many words name it. `everywhere` makes the lookups of the names after it span the whole
 * volume. The words are looked up as a field asks for them, and a Form whose
 * first field is text looks up nothing. Form and lookup outlive the filler. */
int  form_fill_begin(const BoxForm *form, const char *line, BoxFormLookup *lookup,
                     BoxFormFiller **out);
/* The next thing the filler needs, or DONE. Call again after each answer.
 * REFUSED is final: a word nothing takes, `them` with nothing given before
 * beside files that were, a name in a comma list that is no file, `all`
 * answered no, a lookup the volume would not answer, an arrow with no target.
 * Every later step answers REFUSED again; the shell says the sentence. */
int  form_fill_step(BoxFormFiller *filler, BoxFormQuestion *question);
/* ASK_WHICH answered: Choice is one of Choices. */
int  form_fill_choose(BoxFormFiller *filler, file_id_t choice);
/* ASK_FIELD answered: Words as the person typed them, sorted by the rule of
 * the line (a name that means several files asks which). Words that name no
 * file leave a thing field empty and it is asked again with the reason; the
 * line's own word that looked like the file wanted (a name with a dot that
 * was no file here) is let go with the answer. */
int  form_fill_words(BoxFormFiller *filler, const char *words);
/* ASK_SURE answered; no is a REFUSED. */
int  form_fill_sure(BoxFormFiller *filler, bool yes);
/* DONE: field N as filled — its files, or its words (a name and the text
 * are one word each). The arrays live until the next call on the filler. */
int  form_fill_field(BoxFormFiller *filler, uint32_t field, const file_id_t **ids, size_t *id_count,
                     const char *const **words, size_t *word_count);
/* The program's name as typed — word 0 of the Luggage, put before the line
 * the filler was given. Optional; before form_fill_luggage. */
int  form_fill_program(BoxFormFiller *filler, const char *name);
/* The starter will read what the program gives back: the Luggage asks the
 * program for its Souvenir. Without it the program gives back to no one.
 * Optional; before form_fill_luggage. */
int  form_fill_souvenir(BoxFormFiller *filler);
/* DONE: the arrow's file when the program has no `into` — its id, or 0 with
 * *create the name of a file to make first (the shell makes it); none: 0
 * and NULL. *create lives until form_fill_end. */
int  form_fill_arrow(BoxFormFiller *filler, file_id_t *screen, const char **create);
/* DONE: the Luggage — the program's name and Line without the arrow, a NUL,
 * the records — into a heap buffer the caller frees; Screen the arrow's file
 * (the made one, when the shell made it), 0 for none. */
int  form_fill_luggage(BoxFormFiller *filler, file_id_t screen, char **bytes, size_t *length);
void form_fill_end(BoxFormFiller *filler);

#ifdef __cplusplus
}
#endif

#endif /* BOX_FORM_H */
