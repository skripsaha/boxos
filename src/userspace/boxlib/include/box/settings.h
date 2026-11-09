#ifndef BOX_SETTINGS_H
#define BOX_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"

#include "settings_reader.h"

/*
 * Settings — what a person can set in a program, and what they set.
 *
 * What CAN be set the program says once, in its image:
 *
 *     BOX_SETTINGS("setting hours one-of 24 12 default 24 \"how the hours are counted\"\n"
 *                  "setting seconds yes-or-no default yes \"whether the seconds are shown\"\n"
 *                  "setting color color default #E6E6E6 \"the colour of the digits\"\n");
 *
 * (In BoxASM: the directive `setting hours one-of 24 12 default 24 "how the
 * hours are counted"`.) Each line is `setting NAME KIND [what the kind takes]
 * default VALUE [next-start] "what it does"` — the kinds yes-or-no, number
 * from A to B [UNIT], word, text, one-of W1 W2 ... and color #RRGGBB, as
 * src/include/settings_reader.h defines them; the toolchain refuses a line it
 * could not read. The declaration is the one source of what each setting is:
 * the utility settings and help show it, the values are checked against it,
 * and its defaults are what a program has until the person changes them.
 *
 * What the person CHOSE is theirs: the file <program>.settings on the volume,
 * holding only what they changed — `settings clock hours 12` writes it, or
 * they edit it by hand. A program never writes it on its own.
 *
 * The file is found by the program's name everywhere on the volume, not only
 * in the Use Context; with two of that name, the one tagged settings is read,
 * and that is said. A line the person wrote wrong never stops the program:
 * the setting keeps its default, and the line is named among the problems —
 * "clock.settings, line 3: hours wants one of 24, 12; 24 is kept",
 * "clock has no setting hourz — did you mean hours?". A line for a name the
 * program does not declare (another version's) is left in the file.
 *
 * A program reads its own settings by name. The text an accessor answers
 * lives until the settings are read again (settings_reload). Asking for a name the program does not declare, or for a
 * setting as a kind it is not, is the program's mistake: said once, and the
 * answer is false, 0, "" or black.
 */

/* BOX_SETTINGS(text): the program's declaration, text as above without its
 * first line `settings 1`. One per program. It lands in .boxos.settings — in a
 * box the part settings, put into memory at start. */
#define BOX_SETTINGS(Text)                                                     \
    __attribute__((section(".boxos.settings"), used, aligned(1)))              \
    static const char BoxSettingsDeclared[] = "settings 1\n" Text

/* ---- A program's own settings ----------------------------------------- */

bool        settings_yes(const char *name);
int64_t     settings_number(const char *name);
const char *settings_word(const char *name);
const char *settings_text(const char *name);
const char *settings_choice(const char *name);
uint32_t    settings_color(const char *name);     /* 0xRRGGBB */

/* The person's file read again: true when a value changed. */
bool settings_reload(void);

/* ---- Any program's settings: what the utility settings shows and writes -- */

/* One setting: as declared, and what it is now. Strings are NUL-ended. */
typedef struct {
    const char  *Name;
    SettingKind  Kind;
    const char  *What;
    const char  *Default;
    int64_t      From, To;      /* number */
    const char  *Unit;          /* number: "" for none */
    const char  *Choices;       /* one-of: the words apart by blanks */
    bool         NextStart;
    const char  *Value;         /* the person's value when it holds, else the default */
    bool         Yes;           /* yes-or-no */
    int64_t      Number;        /* number */
    uint32_t     Color;         /* color: 0xRRGGBB */
    bool         Chosen;        /* the person's file sets it, and the value holds */
} BoxSetting;

typedef struct {
    char         Program[32];   /* the program's name, its file's stem: clock */
    file_id_t    File;          /* the person's file, 0 when there is none */
    BoxSetting  *Settings;      /* in the declaration's order */
    size_t       Count;
    char       **Problems;      /* sentences: lines of the declaration or of the file not taken */
    size_t       ProblemCount;
    char        *Declared;      /* the declaration's text, and its length */
    size_t       DeclaredSize;
    char        *Chosen;        /* the person's file's bytes, and their count */
    size_t       ChosenSize;
    char        *Why;           /* the last refusal's sentence */
} BoxSettings;

/* The settings of the program in file `program`, its declaration read from
 * its part settings (box) or its .boxos.settings (ELF), the person's file
 * found and read. A program that declares none answers Count 0 and 0.
 * Returns 0 or a negative -error_t. */
int  settings_read(file_id_t program, BoxSettings *out);
void settings_release(BoxSettings *settings);
const BoxSetting *settings_find(const BoxSettings *settings, const char *name);
/* What a setting takes, said for a person: "one of 24, 12", "a whole number
 * from 5 to 40 cells". */
void settings_wants(const BoxSetting *setting, char *out, size_t capacity);
/* Whether `value` fits the setting called `name`: 0, or -ERR_INVALID_ARGUMENT
 * with the sentence in settings->Why — no such setting (and the one meant,
 * when one is near), or what it wants. */
int settings_check(BoxSettings *settings, const char *name, const char *value);

/* The person's file changed by one setting and saved whole, made when there
 * is none (tagged settings) — `value` taken as it is, held to the declaration
 * first. On a refusal *why is the sentence that says why ("hours wants one of
 * 24, 12"), valid until the next call on `settings`. Each reads `settings`
 * again when it changed the file. Returns 0 or a negative -error_t
 * (-ERR_INVALID_ARGUMENT for a refusal). */
int settings_set(BoxSettings *settings, const char *name, const char *value, const char **why);
/* The setting as it came: its lines erased. */
int settings_erase(BoxSettings *settings, const char *name, const char **why);
/* Every setting the program declares as it came: their lines erased —
 * comments, other versions' names and `when` lines stay. */
int settings_clear(BoxSettings *settings);

#ifdef __cplusplus
}
#endif

#endif /* BOX_SETTINGS_H */
