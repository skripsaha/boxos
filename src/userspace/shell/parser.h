#ifndef PARSER_H
#define PARSER_H

#include "shell.h"

int parser_parse(const char* input, parsed_command_t* cmd);

#endif // PARSER_H
