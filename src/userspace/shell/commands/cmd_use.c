#include "commands.h"
#include "../shell.h"
#include "box/io.h"
#include "box/file.h"
#include "box/string.h"

int cmd_use(int argc, char* argv[]) {
    shell_state_t* state = shell_get_state();

    if (argc == 1) {
        state->context_tag_count = 0;
        memset(state->context_tags, 0, sizeof(state->context_tags));

        context_clear();

        shell_update_prompt();

        println("Context cleared");
        return 0;
    }

    context_clear();
    state->context_tag_count = 0;

    for (int i = 1; i < argc && state->context_tag_count < SHELL_MAX_CONTEXT_TAGS; i++) {
        size_t tag_len = strlen(argv[i]);
        if (tag_len > 31) tag_len = 31;

        memcpy(state->context_tags[state->context_tag_count], argv[i], tag_len);
        state->context_tags[state->context_tag_count][tag_len] = '\0';
        state->context_tag_count++;

        context_set(argv[i]);
    }

    shell_update_prompt();

    println("Context set");
    return 0;
}
