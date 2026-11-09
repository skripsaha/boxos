/* life.c — how a program lives, read from its file. */

#include "box/life.h"
#include "box/error.h"
#include "box/memory.h"
#include "box/program_part.h"
#include "box_reader.h"

STATIC_ASSERT((int)PROGRAM_TASK == (int)BOX_LIFE_TASK && (int)PROGRAM_STAYS == (int)BOX_LIFE_STAYS &&
                  (int)PROGRAM_SERVES == (int)BOX_LIFE_SERVES,
              "a program's way of living is the part's word for it");

/* A life part says four words; anything near this is no life part. */
#define LIFE_TEXT_MAX 4096

int program_life(file_id_t file, ProgramLife *out)
{
    char   *text;
    size_t  size;
    BoxLife life;
    out->Living = PROGRAM_TASK;
    out->Rise   = 0;
    int rc = program_part_read(file, "life", LIFE_TEXT_MAX, &text, &size, NULL);
    if (rc <= 0 || !text) return rc < 0 ? rc : 0;
    BoxVerdict verdict = BoxLifeRead((const uint8_t *)text, size, &life, NULL);
    free(text);
    if (verdict != BOX_OK) return -ERR_INVALID_ARGUMENT;
    out->Living = life.Kind;
    out->Rise   = life.Rise;
    return 0;
}
