#ifndef BOX_PROGRAM_PART_H
#define BOX_PROGRAM_PART_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"

/*
 * A program's parts by their names — form, recipe, about, a part of any kind
 * a box carries — read the one way whatever the program is.
 *
 * A box (box_format.h) is read as the kernel reads it, through the one reader
 * (box_reader.h), and never whole: its header, its table and names held to
 * the seal, then the one part asked for, held to its hash. An ELF image keeps
 * the parts it has at all in sections of its own (form in .boxos.form, recipe
 * in .boxos.recipe), read at offsets (box/core/image_section.h).
 *
 * The program that is running reads the parts its box marks to be loaded at
 * start from its own memory (program_part_loaded); every other part of it from
 * its file, cabin_info()->program_file, like any program's.
 */

typedef enum {
    PROGRAM_NONE = 0,   /* the file is no program this library reads */
    PROGRAM_ELF,
    PROGRAM_BOX,
} ProgramKind;

/* What the file is: *Kind a box, an ELF image or no program, told by the file's
 * first bytes alone — nothing of what it is called or tagged. 0, or why it
 * could not be read. */
int program_kind(file_id_t file, ProgramKind *kind);

/* Every program on the volume — every file that is a box or an ELF image, told
 * by its first bytes as program_kind tells one — in file id order, the deleted
 * never met: the volume answers off the files' cards, and no file is read. The
 * count and a malloc'd list the caller frees (NULL for none), or a negative
 * -error_t. */
int program_files(file_id_t **out_ids);

/* The part called Name of the program File. 1: found, *Bytes a heap copy with
 * a NUL after it (NULL when the part is empty), *Size its size. 0: the program
 * has no such part, or the file is no program. Negative: a program that cannot
 * be right — a box whose table does not hold to its seal or whose part does not
 * hold to its hash, a part larger than Limit (-ERR_CORRUPTED); a part kept
 * compressed (-ERR_UNSUPPORTED); a read the volume refused; no memory. *Kind
 * says what the file is; it may be NULL. */
int program_part_read(file_id_t file, const char *name, size_t limit, char **bytes, size_t *size,
                      ProgramKind *kind);

/* The part called Name as this program's own memory holds it: a part its box
 * marks to be loaded at start, named in its table of what is where
 * (box_loaded.h). 1: *Bytes points at it, read-only for as long as the program
 * runs, *Size its size. 0: no such part is in memory — the program is no box,
 * or the part stays in its file. */
int program_part_loaded(const char *name, const uint8_t **bytes, size_t *size);

#ifdef __cplusplus
}
#endif

#endif /* BOX_PROGRAM_PART_H */
