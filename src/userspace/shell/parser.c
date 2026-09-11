
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