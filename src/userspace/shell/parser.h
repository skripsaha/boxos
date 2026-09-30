#ifndef PARSER_H
#define PARSER_H

#include "shell.h"

/* 0 with `cmd` holding the line's words, or -1 with nothing held. */
int  ParserParse(const char *input, ParsedCommand *cmd);
void ParserFree(ParsedCommand *cmd);

#endif /* PARSER_H */
