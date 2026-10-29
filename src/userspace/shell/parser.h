#ifndef PARSER_H
#define PARSER_H

#include "shell.h"

/* 0 with `cmd` holding the line's words, or -1 with nothing held. */
int  ParserParse(const char *input, ParsedCommand *cmd);
void ParserFree(ParsedCommand *cmd);

/* What is left of `line` after its first `words` words, cut as ParserParse
 * cuts them, with the blanks before the next word dropped. */
const char *ParserRest(const char *line, int words);

#endif /* PARSER_H */
