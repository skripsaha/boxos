/*
 * shell.c — BoxOS interactive shell
 *
 * Architecture: Shell → IPC → Display Daemon → VGA
 * Input: line editor with cursor movement and history
 * Commands: built-in table + external utilities via proc_exec
 * Context: tag-based focus via `use` command
 */

#include "shell.h"
#include "line_edit.h"
#include "parser.h"
#include "executor.h"
#include "box/print.h"
#include "box/string.h"
#include "box/ipc.h"
#include "box/core/result.h"
#include "box/system.h"
#include "box/core/notify.h"
#include "box/core/cabin.h"
#include "box/display.h"

static ShellState    g_state;
static LineEditState g_editor;

/* =========================================================================
 * Initialization
 * ========================================================================= */

void ShellInit(void)
{
    memset(&g_state, 0, sizeof(ShellState));
    g_state.running = true;
    memcpy(g_state.prompt, "~ ", 3);

    LineEditInit(&g_editor);

    CabinInfo *ci = cabin_info();

    if (ci->spawner_pid == 0) {
        /* Root shell: spawn display daemon */
        int display_pid = proc_exec("display");
        if (display_pid > 0) {
            Result entry;
            if (receive_wait(&entry, 2000)) {
                io_set_mode(IO_MODE_IPC);
                io_set_display_pid(entry.sender_pid);
            }
        }
    } else {
        /* Nested shell: display already exists — discover it */
        io_set_mode(IO_MODE_IPC);

        /* Drain the args IPC message sent by parent shell */
        Result pending;
        while (receive(&pending)) { }

        /* Discover display daemon PID via PING broadcast */
        uint8_t ping = DISP_CMD_PING;
        broadcast("display", &ping, 1);

        Result entry;
        if (receive_wait(&entry, 2000) && entry.sender_pid != 0) {
            io_set_display_pid(entry.sender_pid);
        }
    }

    clear();
    println("BoxOS Shell v2.0");
    println("Ctrl+Q to exit. Type 'help' for commands.");
    println("");
}

/* =========================================================================
 * Prompt management
 * ========================================================================= */

void ShellUpdatePrompt(void)
{
    if (g_state.context_tag_count > 0) {
        char ctx[SHELL_PROMPT_MAX];
        int pos = 0;
        ctx[pos++] = '[';

        for (uint32_t i = 0; i < g_state.context_tag_count && pos < SHELL_PROMPT_MAX - 5; i++) {
            size_t tag_len = strlen(g_state.context_tags[i]);
            if (pos + (int)tag_len + 2 >= SHELL_PROMPT_MAX - 5) break;

            memcpy(ctx + pos, g_state.context_tags[i], tag_len);
            pos += (int)tag_len;

            if (i < g_state.context_tag_count - 1)
                ctx[pos++] = ',';
        }

        ctx[pos++] = ']';
        ctx[pos++] = ' ';
        ctx[pos++] = '~';
        ctx[pos++] = ' ';
        ctx[pos]   = '\0';
        memcpy(g_state.prompt, ctx, (size_t)pos + 1);
    } else {
        memcpy(g_state.prompt, "~ ", 3);
    }
}

/* =========================================================================
 * Main loop
 * ========================================================================= */

/* =========================================================================
 * TakeSurvey — ask user a yes/no question before irreversible actions
 * ========================================================================= */

static bool TakeSurvey(const char *message)
{
    printf("%color%s [y/n] %color",
           COLOR_YELLOW, message, COLOR_DEFAULT);
    io_flush();

    while (1) {
        int ch = getchar();
        if (ch < 0) continue;
        if (ch == 'y' || ch == 'Y') {
            println("y");
            return true;
        }
        if (ch == 'n' || ch == 'N' || ch == 0x1B) {
            println("n");
            return false;
        }
    }
}

/* =========================================================================
 * Main loop
 * ========================================================================= */

void ShellMainLoop(void)
{
    char input[SHELL_LINE_MAX];

    while (g_state.running) {
        int rc = LineEditRead(&g_editor, g_state.prompt, input, SHELL_LINE_MAX);

        if (rc == LINE_EXIT_REQUEST) {
            if (TakeSurvey("Shut down BoxOS?")) {
                ShellStop();
                break;
            }
            continue;
        }

        if (rc == LINE_EMPTY || rc == LINE_ERROR)
            continue;

        /* Parse */
        ParsedCommand cmd;
        if (ParserParse(input, &cmd) != 0) {
            println("Error: failed to parse command");
            continue;
        }

        if (cmd.argc == 0)
            continue;

        /* Execute */
        int result = ExecutorRun(&cmd);
        if (result != 0) {
            const char *err = ExecutorGetError();
            if (err && err[0] != '\0') {
                printf("%colorError: %s%color\n",
                       COLOR_RED, err, COLOR_DEFAULT);
            }
        }
    }
}

void ShellStop(void)
{
    g_state.running = false;
}

ShellState *ShellGetState(void)
{
    return &g_state;
}

/* =========================================================================
 * Entry point
 * ========================================================================= */

int main(void)
{
    ShellInit();
    ShellMainLoop();
    return 0;
}
