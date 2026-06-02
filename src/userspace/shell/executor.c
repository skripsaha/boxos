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
#include "box/core/result.h"
#include "box/print.h"

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

    /* Build args + context tags into IPC buffer. SHELL_ARGS_BUF_MAX
     * (240 B) bounds the legacy send() payload; if the user's command
     * + tags exceeds that, we ship a partial argv to the child and
     * warn the user rather than silently corrupting their input. */
    ShellState *state = ShellGetState();
    char buf[SHELL_ARGS_BUF_MAX];
    int pos = 0;
    int args_sent = 0;

    buf[pos++] = (char)cmd->argc;
    for (int i = 0; i < cmd->argc; i++) {
        size_t len = strlen(cmd->argv[i]);
        if (pos + (int)len + 1 > SHELL_ARGS_BUF_MAX - 2) break;
        memcpy(buf + pos, cmd->argv[i], len);
        pos += (int)len;
        buf[pos++] = '\0';
        args_sent++;
    }
    /* Fix up the count byte so the child loops over what we actually
     * shipped, not what the user originally typed. */
    buf[0] = (char)args_sent;

    if (args_sent < cmd->argc) {
        printf("%colorWarning:%color shell IPC buffer full, sent %d/%d args\n",
               COLOR_YELLOW, COLOR_DEFAULT, args_sent, cmd->argc);
    }

    /* Append context tags */
    int tags_sent = 0;
    if (pos < SHELL_ARGS_BUF_MAX)
        buf[pos++] = (char)state->context_tag_count;
    for (uint32_t ci = 0; ci < state->context_tag_count; ci++) {
        size_t len = strlen(state->context_tags[ci]);
        if (pos + (int)len + 1 > SHELL_ARGS_BUF_MAX) break;
        memcpy(buf + pos, state->context_tags[ci], len);
        pos += (int)len;
        buf[pos++] = '\0';
        tags_sent++;
    }
    if (tags_sent < (int)state->context_tag_count) {
        printf("%colorWarning:%color sent %d/%u context tags\n",
               COLOR_YELLOW, COLOR_DEFAULT,
               tags_sent, state->context_tag_count);
    }

    send((uint32_t)pid, buf, (uint16_t)pos);

    /* Fast path: child may have exited during send's result_wait. The
     * non-blocking receive() here is intentional — we only want the
     * sentinel if it's already there; otherwise fall through to the
     * polling wait below. */
    {
        Result early;
        if (receive(&early)) {
            if (early.data_length >= 1 && early.data_addr != 0 &&
                *(uint8_t *)(uintptr_t)early.data_addr == SHELL_EXIT_SENTINEL) {
                ShellDrainStaleIpc();
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

    /* Drain leftover IPC (late exit sentinels, stray broadcasts). */
    ShellDrainStaleIpc();

    return 0;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int ExecutorRun(ParsedCommand *cmd)
{
    g_error[0] = '\0';

    /* Drain stale IPC before dispatching so a leftover child-exit
     * sentinel cannot be mis-routed to the new command. */
    ShellDrainStaleIpc();

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
