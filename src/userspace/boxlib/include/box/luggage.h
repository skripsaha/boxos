#ifndef BOX_LUGGAGE_H
#define BOX_LUGGAGE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"


typedef struct {
    const char *bytes;
    uint32_t    length;
} Luggage;

Luggage luggage(void);

uint32_t luggage_word_count(void);

const char *luggage_word(uint32_t index);

const char *luggage_tail(uint32_t from);

uint32_t luggage_cut(char *line, char **words, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif