#ifndef HISTORY_H
#define HISTORY_H

#include "box/defs.h"

/* The lines the person typed at the shell's prompt, each with the Use Context
 * it was typed in, newest last. They live on the volume, in the file `history`
 * (tags shell and whisper), so they outlast the shell and the night, and
 * travel with the medium; with no volume to keep them they live as long as
 * the shell. An answer to a question and an Itinerary's step are not typed at
 * the prompt and are not here. */

/* The file read in, when the volume has one. */
void HistoryLoad(void);

/* `line` typed in `context` (comma-joined tags; NULL or "" for none):
 * remembered, and added to the end of the file. The same line in the same
 * context as the newest is remembered once. */
void HistoryKeep(const char *line, const char *context);

/* The line `back` lines before the newest (0: the newest) and the context it
 * was typed in ("" for none): the history's own strings, valid until the next
 * HistoryKeep. NULL past the oldest. */
const char *HistoryLine(size_t back, const char **context);

#endif /* HISTORY_H */
