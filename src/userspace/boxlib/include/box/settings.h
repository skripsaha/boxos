#ifndef BOX_SETTINGS_H
#define BOX_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"
#include "settings_reader.h"

/*
 * Settings — what a person can set in a program, and what they set: a tree
 * of things, and what each thing has.
 *
 * What CAN be set the program says once, in its image:
 *
 *     BOX_SETTINGS("hours one-of 24 12 default 24 \"how the hours are counted\"\n"
 *                  "seconds on-or-off default on \"whether the seconds are shown\"\n"
 *                  "digits \"the digits of the time\"\n"
 *                  "    color color default #E6E6E6 \"their color\"\n");
 *
 * (In BoxASM: the directive `setting` with a whole path, `setting digits
 * color color default #E6E6E6 "their color"`.) Each line is `NAME KIND [what
 * the kind takes] default VALUE [next-start] "what it does"`, or `NAME "what
 * it holds"` for a thing that only holds the lines indented under it — the
 * kinds on-or-off, number from A to B [UNIT] [or off], word, text, one-of W1
 * W2 ..., color and key, as src/include/settings_reader.h defines them; the
 * toolchain refuses a line it could not read. The declaration is the one
 * source of what each setting is: the utility settings and help show it, the
 * values are checked against it, and its defaults are what a program has
 * until the person changes them.
 *
 * What the person CHOSE is theirs: the file <program>.settings on the volume,
 * holding only what they changed — `settings clock digits color amber` writes
 * it, or they edit it by hand, in blocks or whole paths. A program never
 * writes it on its own.
 *
 *     digits
 *         color  amber
 *     seconds off
 *     [work]
 *         hours 12
 *
 * A setting is named by its path: its names from the top down, apart by
 * blanks — "digits color". The lines of a context's block, [TAGS], are held
 * to the declaration like any other and kept apart from what is set
 * everywhere, in the context they were written for. They hold while the Use
 * Context (box/use.h) holds every tag of the block: of two blocks that both
 * do, the one of more tags — [work, late] over [work] — and of two of as
 * many, the one written last; when none does, what is set everywhere holds.
 *
 * The file is found by the program's name everywhere on the volume, not only
 * in the Use Context; with two of that name, the one tagged settings is read,
 * and that is said. A line the person wrote wrong never stops the program:
 * the setting keeps its value, and the line is named among the problems —
 * "clock.settings, line 3: hours wants one of 24, 12; 24 is kept",
 * "clock.settings, line 4: clock has no setting digts — did you mean
 * digits?". A line for a name the program does not declare (another
 * version's) is left in the file.
 *
 * A program reads its own settings by path, as they hold in the Use Context
 * the settings were last read in. The text an accessor answers lives until
 * the settings are read again (settings_reload, or a watch that saw the file
 * saved or the context change). Asking for a path the program does not
 * declare, or for a setting as a kind it is not, is the program's mistake:
 * said once, and the answer is false, 0, "", black or no key.
 */

/* BOX_SETTINGS(text): the program's declaration, text as above without its
 * first line `settings 1`. One per program. It lands in .boxos.settings — in a
 * box the part settings, put into memory at start. */
#define BOX_SETTINGS(Text)                                                     \
    __attribute__((section(".boxos.settings"), used, aligned(1)))              \
    static const char BoxSettingsDeclared[] = "settings " SETTINGS_VERSION_WORD "\n" Text

/* ---- A program's own settings ----------------------------------------- */

bool        settings_on(const char *path);
int64_t     settings_number(const char *path);     /* 0 for off */
const char *settings_word(const char *path);
const char *settings_text(const char *path);
const char *settings_choice(const char *path);
uint32_t    settings_color(const char *path);      /* 0xRRGGBB */
KeyChord    settings_key(const char *path);        /* key_chord.h; all 0 for none */

/* The person's file and the Use Context read again: true when a value that
 * holds changed. */
bool settings_reload(void);

/* Watching the person's file and the Use Context: the program's settings are
 * read again when the file is saved whole (anchored, as the utility settings
 * and edit save it) — Touch on the program's own name, which a file called
 * <program>.settings wears whoever made it, every file wearing the stem of its
 * name — and when the context changes (Touch on USE_TOUCH_TAG). Begun on the
 * strand that polls it: the events are that strand's, and the program's name
 * is a tag it does not claim for anything else. */
typedef struct SettingsWatch SettingsWatch;

int  settings_watch_begin(SettingsWatch **out);
/* Without waiting: true when the program's settings file was saved or the Use
 * Context changed since the last poll, and a value that holds changed. */
bool settings_watch_poll(SettingsWatch *watch);
void settings_watch_end(SettingsWatch *watch);

/* ---- Any program's settings: what the utility settings shows and writes -- */

/* A value read for a setting: its text as the file wrote it, and what its
 * kind makes of it. */
typedef struct {
    const char *Text;           /* a number's digits, or off; a color as written */
    bool        On;             /* on-or-off */
    int64_t     Number;         /* number: 0 for off */
    uint32_t    Color;          /* color: 0xRRGGBB */
    KeyChord    Key;            /* key */
} BoxSettingValue;

/* One setting: as declared, and what it is everywhere now. Strings are
 * NUL-ended. A thing that only holds others is one too, of the kind
 * SETTING_GROUP, its value empty. */
typedef struct {
    const char     *Path;       /* its names from the top down, apart by one blank: "digits color" */
    const char     *Name;       /* its own name: "color" */
    uint32_t        Depth;      /* how many it is inside: 0 at the top */
    uint32_t        Line;       /* its line in the declaration */
    SettingKind     Kind;
    const char     *What;
    const char     *Default;
    int64_t         From, To;   /* number */
    bool            Off;        /* number: off stands for 0 */
    const char     *Unit;       /* number: "" for none */
    const char     *Choices;    /* one-of: the words apart by blanks */
    bool            NextStart;
    BoxSettingValue Value;      /* everywhere: the person's when it holds, else the default */
    bool            Chosen;     /* the person's file sets it everywhere, and the value holds */
} BoxSetting;

/* A value the person's file sets in a context's block, held to the
 * declaration: the setting Settings[Setting] there, its line in the file —
 * of a setting set twice in one block, the last line. */
typedef struct {
    const char     *Tags;       /* the context's tags as the file writes them: "work, home" */
    size_t          Setting;
    BoxSettingValue Value;
    uint32_t        Line;
} BoxSettingInContext;

typedef struct {
    char                 Program[32];   /* the program's name, its file's stem: clock */
    file_id_t            File;          /* the person's file, 0 when there is none */
    BoxSetting          *Settings;      /* in the declaration's order: a thing before what it holds */
    size_t               Count;
    BoxSettingInContext *InContext;     /* in the file's order */
    size_t               InContextCount;
    char               **Problems;      /* sentences: lines of the declaration or of the file not taken */
    size_t               ProblemCount;
    char                *Declared;      /* the declaration's text, and its length */
    size_t               DeclaredSize;
    char                *Chosen;        /* the person's file's bytes, and their count */
    size_t               ChosenSize;
    char                *Why;           /* the last refusal's sentence */
} BoxSettings;

/* The settings of the program in file `program`, its declaration read from
 * its part settings (box) or its .boxos.settings (ELF), the person's file
 * found and read. A program that declares none answers Count 0 and 0.
 * Returns 0 or a negative -error_t. */
int  settings_read(file_id_t program, BoxSettings *out);
void settings_release(BoxSettings *settings);

/* The setting a path names — names apart by blanks — or NULL. */
const BoxSetting *settings_find(const BoxSettings *settings, const char *path);

/* Which value of the setting Settings[index] holds while the Use Context is
 * `context` — its tags apart by commas or blanks, as use_get says them; NULL
 * or "" for none: the value of the block whose tags the context holds every
 * one of, the block of the most tags, and of those the one written last — or
 * NULL when no block does, and the setting's own Value holds. */
const BoxSettingInContext *settings_in_context(const BoxSettings *settings, size_t index, const char *context);

/* What a setting takes, said for a person: "one of 24, 12", "a whole number
 * from 100 to 5000 ms, or off". */
void settings_wants(const BoxSetting *setting, char *out, size_t capacity);

/* Whether `value` fits the setting at `path`: 0, or -ERR_INVALID_ARGUMENT
 * with the sentence in settings->Why — no such setting (and the one meant,
 * when one is near), a thing that only holds others, or what it wants. */
int settings_check(BoxSettings *settings, const char *path, const char *value);

/* The person's file changed by one setting and saved whole, made when there
 * is none (tagged settings) — `value` taken as it is, held to the declaration
 * first; in the block of the context `tags` (apart by blanks or commas), or
 * everywhere for NULL or "". On a refusal *why is the sentence that says why
 * ("hours wants one of 24, 12"), valid until the next call on `settings`.
 * Each reads `settings` again when it changed the file. Returns 0 or a
 * negative -error_t (-ERR_INVALID_ARGUMENT for a refusal). */
int settings_set(BoxSettings *settings, const char *tags, const char *path, const char *value, const char **why);
/* The setting as it came there: its lines erased — for a thing that only
 * holds others, the lines of every setting inside it. */
int settings_erase(BoxSettings *settings, const char *tags, const char *path, const char **why);
/* Every setting the program declares as it came, in every context: their
 * lines erased — comments and other versions' names stay. */
int settings_clear(BoxSettings *settings);

#ifdef __cplusplus
}
#endif

#endif /* BOX_SETTINGS_H */
