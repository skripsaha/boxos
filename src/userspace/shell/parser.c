/*
 * parser.c — cut a typed line into words for the built-in commands.
 *
 * The cut is the luggage's cut (box/luggage.h): blanks separate, "double
 * quotes" group. One rule, so a built-in sees the same words an external
 * program finds in its Luggage. The line is as long as it was typed and has
 * as many words as it has: the cut counts them first and the word array is
 * made to that count.
 */

#include "parser.h"
#include "box/string.h"
#include "box/memory.h"
#include "box/luggage.h"

int ParserParse(const char *input, ParsedCommand *cmd)
{
    if (!input || !cmd) return -1;
    memset(cmd, 0, sizeof(ParsedCommand));

    size_t len = strlen(input);
    cmd->storage = malloc(len + 1);
    if (!cmd->storage) return -1;

    /* Counted on the copy, which the count cuts; then the copy is laid down
     * again and cut into an array of exactly that many words. */
    memcpy(cmd->storage, input, len + 1);
    uint32_t words = luggage_cut(cmd->storage, NULL, 0);
    memcpy(cmd->storage, input, len + 1);

    if (words > 0) {
        cmd->argv = malloc(sizeof(char *) * words);
        if (!cmd->argv) {
            ParserFree(cmd);
            return -1;
        }
        luggage_cut(cmd->storage, cmd->argv, words);
    }
    cmd->argc = (int)words;
    return 0;
}

void ParserFree(ParsedCommand *cmd)
{
    if (!cmd) return;
    free(cmd->argv);
    free(cmd->storage);
    memset(cmd, 0, sizeof(ParsedCommand));
}
