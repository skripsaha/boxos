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
 * when none does. What it takes
 * for itself — the screen, the scene — it does; what is the kernel's it hands on in the
 * Boarding Pass, a stamp for each the file sets (boarding_pass.h: STARTUP,
 * CORES, KEEP_LOG). Every setting takes effect when the machine starts next.
 * The panel (gangway_panel.c) changes the same five for one start and writes
 * nothing back. BIOS boots read no
 * settings: stage2 stays quiet, and the kernel goes its own way.
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
    "and boot.scene when none does: duppo-word dark animated\"\n"

#endif /* BOXOS_GANGWAY_SETTINGS_H */
