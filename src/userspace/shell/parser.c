#include "parser.h"
#include "box/string.h"

static char *Tokenize(char *str, char **saveptr)
{
    if (!str && !*saveptr) return NULL;

    char *start = str ? str : *saveptr;

    while (*start == ' ' || *start == '\t')
        start++;

    if (*start == '\0') {
        *saveptr = NULL;
        return NULL;
    }

    char *end;
    if (*start == '"') {
        start++;
        end = start;
        while (*end != '\0' && *end != '"')
            end++;
    } else {
        end = start;
        while (*end != '\0' && *end != ' ' && *end != '\t')
            end++;
    }

    if (*end != '\0') {
        *end = '\0';
        *saveptr = end + 1;
    } else {
        *saveptr = NULL;
    }

    return start;
}

int ParserParse(const char *input, ParsedCommand *cmd)
{
    if (!input || !cmd) return -1;

    memset(cmd, 0, sizeof(ParsedCommand));

    size_t input_len = strlen(input);
    if (input_len >= SHELL_LINE_MAX) return -1;

    memcpy(cmd->storage, input, input_len);
    cmd->storage[input_len] = '\0';

    char *saveptr = NULL;
    char *token = Tokenize(cmd->storage, &saveptr);

    while (token != NULL && cmd->argc < SHELL_MAX_ARGS) {
        cmd->argv[cmd->argc++] = token;
        token = Tokenize(NULL, &saveptr);
    }

    return 0;
}
