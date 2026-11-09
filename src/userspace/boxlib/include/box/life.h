#ifndef BOX_LIFE_H
#define BOX_LIFE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"

/*
 * How a program lives — said by the program, read by whoever starts it.
 *
 *     BOX_LIFE("stays");            it lives on by itself: whoever starts it
 *                                   does not wait for its end
 *     BOX_LIFE("serves\nrise");     it serves others, and when it falls the
 *                                   system raises it again
 *
 * The words are box_reader.h's — task, stays, serves, rise — one a line. A
 * box carries them as its part `life` (an ELF image in .boxos.life); a
 * program that says nothing is a task, and is not raised. One per program.
 */

/* How a program lives, as box_reader.h's BoxLifeKind says it. */
typedef enum {
    PROGRAM_TASK = 0,     /* it ends, and whoever starts it waits for its end */
    PROGRAM_STAYS,        /* it lives on by itself: whoever starts it does not wait */
    PROGRAM_SERVES        /* it serves others for as long as the machine runs */
} ProgramLiving;

typedef struct {
    uint8_t Living;   /* ProgramLiving */
    uint8_t Rise;     /* when it falls, the system raises it again */
} ProgramLife;
#define BOX_LIFE(Text)                                                         \
    __attribute__((section(".boxos.life"), used, aligned(1)))                  \
    static const char BoxLifeDeclared[] = "life 1\n" Text "\n"

/* How the program File lives: 0 with *Out as its part says — a task, not
 * raised, when it has none; -ERR_INVALID_ARGUMENT for a part not written as
 * one; or why the part could not be read (program_part.h). */
int program_life(file_id_t file, ProgramLife *out);

#ifdef __cplusplus
}
#endif

#endif /* BOX_LIFE_H */
