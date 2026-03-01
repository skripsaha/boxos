#include "executor.h"
#include "shell.h"
#include "commands/commands.h"
#include "box/string.h"
#include "box/system.h"
#include "box/ipc.h"
#include "box/result.h"
#include "box/io.h"

static char g_error_message[128];

static const shell_command_t g_commands[] = {
    {"use",   cmd_use,   "use [tags]", "Set/clear context"},
    {"exit",  cmd_exit,  "exit",       "Exit shell"},
    {"clear", cmd_clear, "clear",      "Clear screen"},
    {NULL, NULL, NULL, NULL}
};

int executor_run(parsed_command_t *cmd)
{
    g_error_message[0] = '\0';

    // Drain stale IPC from previous commands
    {
        result_entry_t stale;
        while (receive(&stale)) { /* discard */ }
    }

    if (!cmd || cmd->argc == 0) {
        memcpy(g_error_message, "No command", 11);
        return -1;
    }

    const char *command_name = cmd->argv[0];

    // Check builtin commands
    for (int i = 0; g_commands[i].name != NULL; i++) {
        if (strcmp(command_name, g_commands[i].name) == 0) {
            return g_commands[i].handler(cmd->argc, cmd->argv);
        }
    }

    // Try external utility via proc_exec
    int pid = proc_exec(command_name);
    if (pid > 0) {
        // Build args buffer with context tags appended
        shell_state_t* state = shell_get_state();
        char buf[240];
        int pos = 0;
        buf[pos++] = (char)cmd->argc;
        for (int i = 0; i < cmd->argc && pos < 200; i++) {
            size_t len = strlen(cmd->argv[i]);
            if (pos + (int)len + 1 >= 220) break;
            memcpy(buf + pos, cmd->argv[i], len);
            pos += (int)len;
            buf[pos++] = '\0';
        }
        buf[pos++] = (char)state->context_tag_count;
        for (uint32_t ci = 0; ci < state->context_tag_count && pos < 235; ci++) {
            size_t len = strlen(state->context_tags[ci]);
            if (pos + (int)len + 1 >= 240) break;
            memcpy(buf + pos, state->context_tags[ci], len);
            pos += (int)len;
            buf[pos++] = '\0';
        }
        send((uint32_t)pid, buf, (uint16_t)pos);

        result_entry_t entry;
        int timeout_count = 0;
        while (timeout_count < 50) {
            if (receive_wait(&entry, 100)) {
                if (entry.size >= 1 && entry.payload[0] == 0xFE) {
                    break;
                }
                timeout_count = 0;
            } else {
                timeout_count++;
            }
        }
        return 0;
    }

    // Command not found
    memcpy(g_error_message, "Unknown command: ", 17);
    size_t cmd_len = strlen(command_name);
    if (cmd_len > 100) cmd_len = 100;
    memcpy(g_error_message + 17, command_name, cmd_len);
    g_error_message[17 + cmd_len] = '\0';

    return -1;
}

const char *executor_get_error(void)
{
    return g_error_message;
}
