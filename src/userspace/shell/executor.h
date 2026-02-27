#ifndef EXECUTOR_H
#define EXECUTOR_H

#include "shell.h"

// Execute parsed command
// Returns: 0 on success, non-zero on error
int executor_run(parsed_command_t* cmd);

// Get last error message
const char* executor_get_error(void);

#endif // EXECUTOR_H
