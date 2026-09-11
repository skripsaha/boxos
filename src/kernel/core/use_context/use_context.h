#ifndef USE_CONTEXT_H
#define USE_CONTEXT_H

#include "ktypes.h"
#include "error.h"

struct process_t;


error_t UseContextSet(const char *list, bool intern);
void    UseContextClear(void);

size_t  UseContextTags(char *buf, size_t cap, uint32_t *out_count);

error_t UseContextTagArray(char **out_block, const char ***out_ptrs, uint32_t *out_count);
bool    UseContextIsSet(void);

bool    UseContextMatches(const struct process_t *proc);

void    UseContextRemember(bool *remembered);

void    UseContextRecall(void);

void    UseContextUnbind(void);

void    UseContextBindTag(const char *tag, uint16_t tag_id);

void    UseContextInit(void);
void    UseContextShutdown(void);

#endif