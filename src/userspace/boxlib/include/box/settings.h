#ifndef BOX_SETTINGS_H
#define BOX_SETTINGS_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"

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
 */

/* BOX_SETTINGS(text): the program's declaration, text as above without its
 * first line `settings 1`. One per program. It lands in .boxos.settings — in a
 * box the part settings, put into memory at start. */
#define BOX_SETTINGS(Text)                                                     \
    __attribute__((section(".boxos.settings"), used, aligned(1)))              \
    static const char BoxSettingsDeclared[] = "settings 1\n" Text

#ifdef __cplusplus
}
#endif

#endif /* BOX_SETTINGS_H */
