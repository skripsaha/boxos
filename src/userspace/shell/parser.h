#ifndef PARSER_H
#define PARSER_H

#include "shell.h"

// Parse input string into command structure
// Returns: 0 on success, -1 on error
int parser_parse(const char* input, parsed_command_t* cmd);

#endif // PARSER_H
