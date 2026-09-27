/*
 * quilltest.h — what the two halves of quilltest share.
 *
 * quilltest.c holds the red tests of the Quill write core, the table of
 * cases and the one way in; quilltest_powercut.c holds the modes the
 * power-cut stand drives (tools/powercut.sh). Both say their verdict in the
 * same four words and both ask the volume for files by name.
 */
#ifndef QUILLTEST_H
#define QUILLTEST_H

#include "box/types.h"

typedef enum { QUILL_PASS, QUILL_DEFECT, QUILL_PENDING, QUILL_FAIL } QuillVerdict;

/* The name up to its last dot: the tag TagFS stamps on every file. */
void     QuillStem(const char *name, char *out, size_t cap);
/* Every file called `name`, whatever Use Context is set: how many there are
 * (the first `max` in ids), or a negative -error_t. */
int      QuillFindEverywhere(const char *name, uint32_t *ids, int max);
/* A fresh, empty `name` on the whole volume — every earlier one deleted —
 * and its file id, or <= 0. */
int      QuillCreate(const char *name);
/* fwrite, asked again while the port answers ERR_BUSY; the final answer. */
int64_t  QuillWrite(uint32_t fid, uint64_t off, const void *buf, size_t len);
/* The size file_info reports, 0 when it will not answer. */
uint64_t QuillSize(uint32_t fid);

/* The power-cut modes, quilltest_powercut.c. */
__attribute__((noreturn)) QuillVerdict QuillHog(const char *const *args);
QuillVerdict QuillSettle(const char *const *args);
QuillVerdict QuillHogged(const char *const *args);
QuillVerdict QuillVerify(const char *const *args);
QuillVerdict QuillChurn(const char *const *args);
QuillVerdict QuillRace(const char *const *args);

#endif /* QUILLTEST_H */
