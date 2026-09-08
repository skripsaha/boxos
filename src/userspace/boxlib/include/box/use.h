#ifndef BOX_USE_H
#define BOX_USE_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"

/*
 * Use Context — what the person at this machine is doing, said in tags.
 *
 * One per machine, kept by the kernel. The shell sets it when the user says
 * `use code cpp`; from then on every station of the system reads it and
 * adjusts itself: a program whose cabin wears every context tag runs in the
 * scheduler's context tier, a file query is narrowed to the context and a
 * created file is stamped with it (see query_everywhere / create_everywhere
 * in box/file.h for the whole-volume spellings). Any program may ask what
 * the user is doing; only the shell and programs with system authority may
 * say it, because the context is the user's, not a program's.
 *
 * A tag list is comma-separated: "code,cpp" or "code, project:alpha". Blanks
 * around a tag are dropped and "key:" means the bare "key".
 */

/*
 * The volume remembers the context. What is said with use_set / use_clear is
 * written to the mounted volume, and a machine that boots with that volume
 * takes it up again — what one was doing is still what one is doing after
 * the night, and it travels with the medium. `remembered` (optional) receives
 * whether the volume now holds exactly this context: false with no volume
 * up, a medium that will not take the write, or a context longer than the
 * volume's record holds. The context is set either way; the person is told,
 * never refused.
 */

/* Replace the context with the tags of `tags`. NULL or "" clears. Returns 0
 * or a negative -error_t: -ERR_ACCESS_DENIED without system authority,
 * -ERR_INVALID_ARGUMENT for a key or value longer than a tag may be. */
int use_set(const char *tags, bool *remembered);

/* Clear the context. Returns 0 or a negative -error_t. */
int use_clear(bool *remembered);

/*
 * The context as a comma-joined list written into `buf`, NUL-terminated.
 * Returns the number of tags written (0 = no context) or a negative
 * -error_t. `needed` (optional) receives the byte length the whole list takes
 * including its NUL, so a caller whose buffer was too small can size one;
 * such a buffer holds as many whole tags as fit, never a torn one. `buf` may
 * be NULL with `cap` 0 to ask the size alone.
 */
int use_get(char *buf, size_t cap, size_t *needed);

#ifdef __cplusplus
}
#endif

#endif /* BOX_USE_H */
