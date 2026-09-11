#ifndef TOUCH_LOGBOOK_H
#define TOUCH_LOGBOOK_H

#include "touch.h"


#define TOUCH_TAG_KERNEL_BIT   ((TouchTag)0x8000u)

#define TOUCH_LOGBOOK_MAX_INDEX 0x7FFEu

static inline bool TouchTagIsKernel(TouchTag tag_id)
{
    return tag_id != TOUCH_TAG_INVALID && (tag_id & TOUCH_TAG_KERNEL_BIT) != 0;
}

void TouchLogbookResolve(const char *tag, TouchTag *out_full, TouchTag *out_bare);

TouchTag TouchLogbookIntern(const char *tag);

void TouchLogbookLookup(const char *tag, TouchTag *out_full, TouchTag *out_bare);

const char *TouchLogbookName(TouchTag tag_id, const char **out_value);

uint32_t TouchLogbookCount(void);

#endif