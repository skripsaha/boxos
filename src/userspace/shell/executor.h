#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "shell.h"

/* `line` is what the person typed, whole: a built-in gets the cut words, an
 * external program gets the line itself as its Luggage. */
int         ExecutorRun(ParsedCommand *cmd, const char *line);
const char *ExecutorGetError(void);
/* The line the running command was typed as, whole; a built-in that wants
 * its words as typed rather than cut reads it here. */
const char *ExecutorLine(void);

#endif /* EXECUTOR_H */
