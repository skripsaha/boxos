/*
 * parser.c — cut a typed line into words for the built-in commands.
 *
 * The cut is the luggage's cut (box/luggage.h): blanks separate, "double
 * quotes" group. One rule, so a built-in sees the same words an external
 * program finds in its Luggage.
 */

#include "parser.h"
#include "box/string.h"
#include "box/luggage.h"

int ParserParse(const char *input, ParsedCommand *cmd)
{
    if (!input || !cmd) return -1;

    memset(cmd, 0, sizeof(ParsedCommand));

    size_t input_len = strlen(input);
    if (input_len >= SHELL_LINE_MAX) return -1;
    memcpy(cmd->storage, input, input_len);
    cmd->storage[input_len] = '\0';

    uint32_t words = luggage_cut(cmd->storage, cmd->argv, SHELL_MAX_ARGS);
    cmd->argc = (int)(words > SHELL_MAX_ARGS ? SHELL_MAX_ARGS : words);
    return 0;
}
