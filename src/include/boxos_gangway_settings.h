#ifndef BOXOS_GANGWAY_SETTINGS_H
#define BOXOS_GANGWAY_SETTINGS_H

/*
 * boxos_gangway_settings.h — what of Gangway, the UEFI loader, a person may
 * set: its declaration, in the grammar every program's is (settings_reader.h),
 * and the file the person's choices are kept in on the volume.
 *
 * Gangway is no program on the volume and has no box to carry a declaration
 * in, so the declaration is here: Gangway reads the person's file by it at
 * every start, and the utility settings shows and changes the file by it
 * (`settings gangway`, `settings gangway cores 2`) — one text, the one account
 * of what each setting is. The file is the person's alone, as every
 * <program>.settings is: it holds only what was changed, and none of it is
 * written into the loader.
 *
 * Gangway finds the file by its name, never by a tag (the Lectern,
 * gangway_lectern_called.c); the scene it starts with by its name too —
 * boot.scene — unless the person's `scene` names tags: then of the scenes
 * (*.scene) that wear every one of them, one picked by chance, and boot.scene
 * when none does. What it takes for itself — the screen, the scene, the
 * panel's letters, colours and logo (text, theme, colors, logo) — it does;
 * what is the kernel's it hands on in the Boarding Pass, a stamp for each the
 * file sets (boarding_pass.h: STARTUP, CORES, KEEP_LOG). Every setting takes
 * effect when the machine starts next. The panel (gangway_panel.c) changes
 * the start's and its own look for one start — every one but the colours —
 * and writes nothing back. BIOS boots read no settings: stage2 stays quiet,
 * and the kernel goes its own way.
 */

#define GANGWAY_SETTINGS_PROGRAM "gangway"
#define GANGWAY_SETTINGS_FILE    GANGWAY_SETTINGS_PROGRAM ".settings"

#define GANGWAY_SETTINGS                                                                                       \
    "settings\n"                                                                                               \
    "screen word default auto next-start \"the screen's size while the machine starts: auto, the monitor's "   \
    "own, or as wide x as tall, 1920x1080\"\n"                                                                 \
    "startup one-of quiet all default quiet next-start \"what the screen shows while the machine starts: "     \
    "quiet, or all the kernel says\"\n"                                                                        \
    "cores number from 1 to 256 or all default all next-start \"how many processors the kernel starts: all, "  \
    "or so many\"\n"                                                                                           \
    "keep-log on-or-off default off next-start \"what the machine says kept through a reset, for lastsaid to " \
    "read after it\"\n"                                                                                       \
    "scene text default on next-start \"the scene shown while the machine starts, played until it is ready "  \
    "for you: on (boot.scene), off, or tags — each start one of the scenes that wear them all, by chance, "     \
    "and boot.scene when none does: duppo-word dark animated\"\n"                                            \
    "text \"the letters of the panel and its pages\"\n"                                                        \
    "    size one-of auto 16 24 32 default auto next-start \"how tall they are: auto for the screen, or 16, "   \
    "24 or 32 px\"\n"                                                                                         \
    "    font one-of regular bold default regular next-start \"their face: JetBrains Mono NL, regular or "       \
    "bold\"\n"                                                                                                \
    "theme one-of dark light default dark next-start \"the panel's colours: dark or light — a colour you set "  \
    "below stands over it\"\n"                                                                                \
    "colors \"the panel's colours, each over the theme's; one you set none of is the theme's — a name or "       \
    "#RRGGBB\"\n"                                                                                             \
    "    page color default black next-start \"behind everything\"\n"                                           \
    "    text color default light-gray next-start \"the rows and the pages' text\"\n"                          \
    "    title color default white next-start \"a page's title\"\n"                                             \
    "    hint color default dark-gray next-start \"the hints, the marks and the rules\"\n"                      \
    "    choice color default #CB5D87 next-start \"the chosen row's plashka: the light falls down it as down "  \
    "Duppo's sphere\"\n"                                                                                      \
    "    note color default #F2A0BF next-start \"a value changed, what is said under the logo, a note\"\n"     \
    "logo on-or-off default on next-start \"Duppo's logo beside the panel; off, the panel stands in the "     \
    "middle\"\n"

#endif /* BOXOS_GANGWAY_SETTINGS_H */
