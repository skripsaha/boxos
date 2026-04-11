#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "shell.h"

int executor_run(parsed_command_t *cmd);
const char *executor_get_error(void);

#endif // EXECUTOR_H
