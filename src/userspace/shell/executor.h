#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "shell.h"

int         ExecutorRun(ParsedCommand *cmd, const char *line);
const char *ExecutorGetError(void);

#endif