#include "parser.h"
#include "box/string.h"

// Simple tokenizer (splits by spaces)
static char* tokenize(char* str, char** saveptr) {
    if (!str && !*saveptr) {
        return NULL;
    }

    char* start = str ? str : *saveptr;

    // Skip leading spaces
    while (*start == ' ' || *start == '\t') {
        start++;
    }

    if (*start == '\0') {
        *saveptr = NULL;
        return NULL;
    }

    // Find end of token
    char* end = start;
    while (*end != '\0' && *end != ' ' && *end != '\t') {
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

int parser_parse(const char* input, parsed_command_t* cmd) {
    if (!input || !cmd) {
        return -1;
    }

    memset(cmd, 0, sizeof(parsed_command_t));

    size_t input_len = strlen(input);
    if (input_len >= SHELL_MAX_INPUT) {
        return -1;
    }

    memcpy(cmd->arg_storage, input, input_len);
    cmd->arg_storage[input_len] = '\0';

    // Tokenize
    char* saveptr = NULL;
    char* token = tokenize(cmd->arg_storage, &saveptr);

    while (token != NULL && cmd->argc < SHELL_MAX_ARGS) {
        cmd->argv[cmd->argc++] = token;
        token = tokenize(NULL, &saveptr);
    }

    return 0;
}
