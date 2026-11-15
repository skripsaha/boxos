#ifndef BOX_CORE_SETTINGS_INTERNAL_H
#define BOX_CORE_SETTINGS_INTERNAL_H

/*
 * What the parts of BoxLib's settings share (box/settings.h): the settings
 * put together from their bytes (settings_read.c, no volume — the host stand
 * builds it), the person's file on the volume (settings_file.c), and a
 * program's own settings (settings_own.c).
 */

#include "box/settings.h"
#include "settings_chosen.h"   /* SETTINGS_FILE_MAX */

#define SETTINGS_SAYING_MAX  256u     /* the longest sentence said of a setting */

/* out->Settings and out->Problems from out->Declared and out->Chosen, for the
 * program out->Program. Returns 0 or -ERR_NO_MEMORY. */
int settings_assemble(BoxSettings *out);

/* The settings put together again from out->Chosen as it is now — the
 * declaration and the program's name kept. Returns 0 or -ERR_NO_MEMORY. */
int settings_reassemble(BoxSettings *out);

/* A sentence added to out->Problems. Returns 0 or -ERR_NO_MEMORY. */
int settings_say(BoxSettings *out, const char *text);

/* The sentence a refusal is, kept in out->Why. Returns -ERR_INVALID_ARGUMENT,
 * or -ERR_NO_MEMORY when it could not be kept. */
int settings_refuse(BoxSettings *out, const char *why);

/* The person's file of the program `program`: found everywhere on the volume
 * by its name, <program>.settings, deleted ones not met, with two the first
 * made; *file 0 when there is none. *saying, when not NULL, is a
 * sentence to add to the problems (two files of that name), the caller's to
 * free. Returns 0 or a negative -error_t. */
int settings_file_find(const char *program, file_id_t *file, char **saying);

/* The whole of a file, at most SETTINGS_FILE_MAX bytes, into a heap buffer
 * the caller frees. Returns 0 or a negative -error_t. */
int settings_file_read(file_id_t file, char **bytes, size_t *size);

/* The program's name, its file's name up to the last dot, into out. Returns
 * 0, or -ERR_INVALID_ARGUMENT when the name leaves none or too long a one. */
int settings_program_name(file_id_t program, char out[32]);

#endif /* BOX_CORE_SETTINGS_INTERNAL_H */
