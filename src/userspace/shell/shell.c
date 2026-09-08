/*
 * shell.c — BoxOS interactive shell
 *
 * Architecture: output rides the shell's console lane (Brook "console:N")
 * to the display daemon; input requests (READLINE) still travel as IPC.
 * Input: readline — boxlib's line editor over the console's ear
 * Commands: built-in table + external utilities via proc_exec
 * Use Context: the user's tag-based focus via `use`, kept by the kernel
 */

#include "shell.h"
#include "parser.h"
#include "executor.h"
#include "box/print.h"
#include "box/string.h"
#include "box/ipc.h"
#include "box/core/result.h"
#include "box/system.h"
#include "box/use.h"
#include "box/core/notify.h"
#include "box/core/cabin.h"

static ShellState    g_state;

/* Implementation lives in the main-loop section so the body is next
 * to its primary user; the prototype in shell.h makes it visible to
 * executor.c which also drains between user operations. */

/* =========================================================================
 * Initialization
 * ========================================================================= */

void ShellInit(void)
{
    memset(&g_state, 0, sizeof(ShellState));
    g_state.running = true;
    memcpy(g_state.prompt, "~ ", 3);


    CabinInfo *ci = cabin_info();

    /* The display daemon is an autostart utility the kernel launches from the
     * volume. The shell never looks for it: its first print asks for a console
     * lane, the kernel says at once whether anyone wears the display tag, and
     * a daemon that exists answers when it reaches its loop — or dies, which
     * the lane wait sees. Either outcome decides IPC or direct VGA in boxlib
     * (lane_fail), on facts and without a clock. The shell never spawns a
     * daemon of its own either: a second daemon aliases on the tag.
     *
     * A nested shell (spawner_pid != 0) first drains what its spawner left
     * in the mailbox, so nothing stale is read as a reply later. */
    io_set_mode(IO_MODE_IPC);

    if (ci->spawner_pid != 0) {
        ShellDrainStaleIpc();
    }

    /* The context this shell was born into — a nested shell inherits the
     * user's `use`, because the context is the machine's, not a shell's. */
    ShellUpdatePrompt();

    clear();
    println("BoxOS Shell v2.0");
    println("Type 'help' for commands, 'exit' to quit.");
    println("");
}

/* =========================================================================
 * Prompt management
 * ========================================================================= */

/*
 * The prompt says what the kernel holds as the Use Context — "[code,cpp] ~ " —
 * read fresh each time rather than kept as a copy of what this shell typed, so
 * a nested shell shows the context it was born into and a context set by a
 * system program shows up too. The kernel writes as many whole tags as the
 * prompt has room for; a context longer than the prompt is shown to that
 * point and never torn mid-tag.
 */
void ShellUpdatePrompt(void)
{
    /* "[" + list + "] ~ " + NUL */
    char list[SHELL_PROMPT_MAX - 5];
    int  tags = use_get(list, sizeof(list), NULL);

    if (tags > 0) {
        size_t len = strlen(list);
        g_state.prompt[0] = '[';
        memcpy(g_state.prompt + 1, list, len);
        memcpy(g_state.prompt + 1 + len, "] ~ ", 5);
    } else {
        memcpy(g_state.prompt, "~ ", 3);
    }
}

/* =========================================================================
 * Main loop
 * ========================================================================= */

/*
 * ShellDrainStaleIpc — drop everything sitting in the IPC mailbox.
 *
 * Nothing the shell waits for arrives as IPC any more: a spawned child sends
 * it NOTHING on exit (its death is a state, process_gone), and the console
 * grant is awaited by the lane itself. What can still sit in the mailbox is
 * what a spawner left behind for a nested shell, and that is drained once,
 * at birth, so it is never read as a reply later.
 */
void ShellDrainStaleIpc(void)
{
    Result drain;
    while (receive(&drain)) { }
}

void ShellMainLoop(void)
{
    char input[SHELL_LINE_MAX];

    while (g_state.running) {
        print(g_state.prompt);
        io_flush();

        int len = readline(input, SHELL_LINE_MAX);
        if (len <= 0)
            continue;

        /* Parse */
        ParsedCommand cmd;
        if (ParserParse(input, &cmd) != 0) {
            println("Error: failed to parse command");
            continue;
        }

        if (cmd.argc == 0)
            continue;

        /* Execute — the cut words for a built-in, the line itself for a
         * program, which carries it as its Luggage. */
        int result = ExecutorRun(&cmd, input);
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
