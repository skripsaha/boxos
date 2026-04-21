/*
 * executor.c — BoxOS shell command executor
 *
 * Built-in commands are looked up in g_commands[].
 * External commands are spawned via proc_exec() and tracked via IPC.
 */

#include "executor.h"
#include "shell.h"
#include "commands/commands.h"
#include "box/string.h"
#include "box/system.h"
#include "box/ipc.h"
#include "box/result.h"
#include "box/io.h"

static char g_error[SHELL_ERROR_MAX];

/* =========================================================================
 * Command table — all built-in commands registered here
 * ========================================================================= */

const ShellCommand g_commands[] = {
    {"help",  cmd_help,  "help",       "Show available commands"},
    {"use",   cmd_use,   "use [tags]", "Set/clear context tags"},
    {"exit",  cmd_exit,  "exit",       "Exit shell (or Ctrl+Q)"},
    {"clear", cmd_clear, "clear",      "Clear screen"},
    {NULL, NULL, NULL, NULL}
};

/* =========================================================================
 * External command execution
 * ========================================================================= */

static int RunExternal(const char *name, ParsedCommand *cmd)
{
    int pid = proc_exec(name);
    if (pid <= 0) return -1;

    /* Build args + context tags into IPC buffer */
    ShellState *state = ShellGetState();
    char buf[SHELL_ARGS_BUF_MAX];
    int pos = 0;

    buf[pos++] = (char)cmd->argc;
    for (int i = 0; i < cmd->argc; i++) {
        size_t len = strlen(cmd->argv[i]);
        if (pos + (int)len + 1 > SHELL_ARGS_BUF_MAX - 2) break;
        memcpy(buf + pos, cmd->argv[i], len);
        pos += (int)len;
        buf[pos++] = '\0';
    }

    /* Append context tags */
    if (pos < SHELL_ARGS_BUF_MAX)
        buf[pos++] = (char)state->context_tag_count;
    for (uint32_t ci = 0; ci < state->context_tag_count; ci++) {
        size_t len = strlen(state->context_tags[ci]);
        if (pos + (int)len + 1 > SHELL_ARGS_BUF_MAX) break;
        memcpy(buf + pos, state->context_tags[ci], len);
        pos += (int)len;
        buf[pos++] = '\0';
    }

    send((uint32_t)pid, buf, (uint16_t)pos);

    /* Fast path: child may have exited during send's result_wait */
    {
        Result early;
        if (receive(&early)) {
            if (early.data_length >= 1 && early.data_addr != 0 &&
                *(uint8_t *)(uintptr_t)early.data_addr == SHELL_EXIT_SENTINEL) {
                Result drain;
                while (receive(&drain)) { }
                return 0;
            }
        }
    }

    /* Wait for child exit sentinel */
    Result entry;
    int idle_iters = 0;

    while (1) {
        if (receive_wait(&entry, SHELL_CHILD_POLL_MS)) {
            if (entry.data_length >= 1 && entry.data_addr != 0 &&
                *(uint8_t *)(uintptr_t)entry.data_addr == SHELL_EXIT_SENTINEL)
                break;
            idle_iters = 0;
        } else {
            idle_iters++;
            if (idle_iters >= SHELL_CHILD_DEAD_ITERS) {
                idle_iters = 0;
                proc_info_t info;
                if (proc_info((uint16_t)pid, &info) != OK ||
                    info.state >= PROC_STATE_TERMINATED)
                    break;
            }
        }
    }

    /* Drain leftover IPC */
    {
        Result drain;
        while (receive(&drain)) { }
    }

    return 0;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int ExecutorRun(ParsedCommand *cmd)
{
    g_error[0] = '\0';

    /* Drain stale IPC */
    {
        Result stale;
        while (receive(&stale)) { }
    }

    if (!cmd || cmd->argc == 0) {
        memcpy(g_error, "No command", 11);
        return -1;
    }

    const char *name = cmd->argv[0];

    /* Check built-in commands */
    for (int i = 0; g_commands[i].name != NULL; i++) {
        if (strcmp(name, g_commands[i].name) == 0)
            return g_commands[i].handler(cmd->argc, cmd->argv);
    }

    /* Try external utility */
    if (RunExternal(name, cmd) == 0)
        return 0;

    /* Not found */
    size_t name_len = strlen(name);
    if (name_len > SHELL_ERROR_MAX - 20) name_len = SHELL_ERROR_MAX - 20;
    memcpy(g_error, "Unknown command: ", 17);
    memcpy(g_error + 17, name, name_len);
    g_error[17 + name_len] = '\0';
    return -1;
}

const char *ExecutorGetError(void)
{
    return g_error;
}
