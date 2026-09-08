#ifndef USE_CONTEXT_H
#define USE_CONTEXT_H

#include "ktypes.h"
#include "error.h"

struct process_t;

/*
 * Use Context — what the person at this machine is doing, said in tags.
 *
 * One per machine, owned by the kernel, set by the user's shell (`use code
 * cpp`) or by a program with system authority. It is not a property of a
 * process or of a cabin: it is the ship's declared condition, and every station
 * reads it and adjusts itself. Two stations do today. The scheduler runs a
 * strand whose cabin wears every context tag in the context tier
 * (SCHED_PRIO_CONTEXT). TagFS narrows a query to the context and stamps a
 * created file with it, unless the caller asked for the whole volume. A later
 * station asks the same question through the same door and needs nothing
 * added here.
 *
 * TAGS, NOT NUMBERS. A tag is a name, `key` or `key:value`. Its number is a
 * page in one particular book, and this machine keeps three books: the mounted
 * volume's registry, the kernel's Logbook, MemTag's registry. So the context
 * holds the NAMES, and asks a book for numbers only when it needs that book's.
 * The volume's numbers are cached here for the one reader that can take
 * neither a lock nor the volume's door — the scheduler. They are bound when the
 * context is set with a volume up, re-bound when a volume is mounted, dropped
 * when the volume's registry is cleared away, and a tag the volume had no
 * number for yet is bound the moment a cabin interns it (UseContextBindTag),
 * so a process tagged after `use` still lands in the context tier.
 */

/*
 * Replace the context with the tags of a comma-separated list — "code,cpp" or
 * "code, project:alpha". An empty list clears. Each tag is normalised: blanks
 * around it are dropped, "key:" with nothing after the colon is the bare "key".
 * A key or a value longer than the volume registry's on-disk field (255 bytes)
 * is refused with ERR_INVALID_ARGUMENT and the context is left as it was.
 *
 * `intern` says whether a tag the mounted volume does not know yet may be
 * entered into its registry. The user's own `use` says yes: a tag is created
 * the way `create` would create it. A reader that must not write to the medium
 * says no and gets a context whose unknown tags simply match nothing until
 * something interns them.
 */
error_t UseContextSet(const char *list, bool intern);
void    UseContextClear(void);

/*
 * The context as names: one NUL-terminated tag after another, copied into
 * `buf`. Returns the byte length the whole answer takes, so a caller whose
 * buffer was too small can size one; writes as many whole tags as fit and
 * reports how many in *out_count. `buf` may be NULL with `cap` 0 to ask the
 * size alone.
 */
size_t  UseContextTags(char *buf, size_t cap, uint32_t *out_count);

/*
 * The same names as an array, for a caller that hands tags to a query: one
 * kmalloc'd block of NUL-terminated names and a kmalloc'd array of pointers
 * into it. Both NULL with count 0 when no context is set; the caller frees
 * both with kfree.
 */
error_t UseContextTagArray(char **out_block, const char ***out_ptrs, uint32_t *out_count);
bool    UseContextIsSet(void);

/*
 * The scheduler's question: does this strand's cabin wear EVERY tag of the
 * context? Lock-free on its fast path. False when no context is set, when
 * there is no volume, and when the context has a tag this volume has no
 * number for yet — nothing can wear a tag that does not exist.
 */
bool    UseContextMatches(const struct process_t *proc);

/*
 * The volume's book changed under the cache. Rebind: look every tag up in the
 * registry that was just mounted — lookup only, because mounting somebody's
 * medium must not write into it. Unbind: that registry is being freed, so its
 * numbers mean nothing from here on.
 */
void    UseContextRebind(void);
void    UseContextUnbind(void);

/*
 * A cabin just interned `tag` and was given `tag_id` for it. If the context
 * holds this tag and had no number for it, this is the number. One relaxed
 * load when nothing is waiting, which is almost always.
 */
void    UseContextBindTag(const char *tag, uint16_t tag_id);

void    UseContextInit(void);
void    UseContextShutdown(void);

#endif /* USE_CONTEXT_H */
