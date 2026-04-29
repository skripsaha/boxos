#include "commands.h"
#include "../shell.h"
#include "box/print.h"
#include "box/storage.h"
#include "box/string.h"

int cmd_use(int argc, char *argv[])
{
    ShellState *state = ShellGetState();

    if (argc == 1) {
        state->context_tag_count = 0;
        memset(state->context_tags, 0, sizeof(state->context_tags));
        context_clear();
        ShellUpdatePrompt();
        println("Context cleared");
        return 0;
    }

    context_clear();
    state->context_tag_count = 0;

    for (int i = 1; i < argc && state->context_tag_count < SHELL_MAX_CONTEXT_TAGS; i++) {
        size_t tag_len = strlen(argv[i]);
        if (tag_len > SHELL_CONTEXT_TAG_LEN - 1)
            tag_len = SHELL_CONTEXT_TAG_LEN - 1;

        memcpy(state->context_tags[state->context_tag_count], argv[i], tag_len);
        state->context_tags[state->context_tag_count][tag_len] = '\0';
        state->context_tag_count++;

        context_set(argv[i]);
    }

    ShellUpdatePrompt();
    println("Context set");
    return 0;
}
