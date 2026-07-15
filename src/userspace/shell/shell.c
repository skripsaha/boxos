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

    LineEditInit(&g_editor);

    CabinInfo *ci = cabin_info();

    /* Display daemon is now an autostart utility (kernel main.c launches
     * any TagFS file tagged "autostart"+"utility|app" at boot). The root
     * shell may still come up before display has registered, in which case
     * we fall back to spawning a private one. Either way, discovery is via
     * a single PING broadcast rather than an unconditional proc_exec —
     * spawning a second copy aliases on the "display" tag and the duplicate
     * page-faults during init.
     *
     * Nested shells (spawner_pid != 0) skip the parent's args IPC drain
     * before doing the PING, so they don't consume their first user
     * notification by mistake. */
    io_set_mode(IO_MODE_IPC);

    if (ci->spawner_pid != 0) {
        ShellDrainStaleIpc();
    }

    /* Discovery: PING the "display" tag and pick up the first responder.
     *
     * The original 500 ms timeout was too aggressive — on a heavily-loaded
     * 16-core boot (each AP racing through init), the autostart display
     * daemon hadn't yet reached its `receive_wait` loop when this PING
     * was issued, so PING went unanswered → shell concluded "no display"
     * → spawned a second daemon → both daemons survived → every later
     * `broadcast("display", ...)` hit both → the user saw duplicated
     * banners and interleaved characters in the serial mirror (the
     * 2026-05-14 STRICT-mode regression).
     *
     * Three-stage discovery:
     *   1. fast PING with a generous wait (covers normal cold boot);
     *   2. one retry — display may have responded to *another* process'
     *      broadcast in the meantime, so we re-PING ourselves;
     *   3. only if both rounds fail and we have no parent that could
     *      have spawned us a display, fall back to proc_exec — and even
     *      then we wait for *that* spawn's reply, never assuming the
     *      original tag is silent forever. */
    Result entry;
    bool   discovered = false;

    for (int round = 0; !discovered && round < 2; round++) {
        uint8_t ping = DISP_CMD_PING;
        broadcast("display", &ping, 1);
        if (receive_wait(&entry, 2500) && entry.sender_pid != 0) {
            io_set_display_pid(entry.sender_pid);
            discovered = true;
        }
    }

    if (!discovered && ci->spawner_pid == 0) {
        /* Historical: spawned a "last-resort" display via proc_exec here.
         * That was the root cause of the post-2026-05-15 "first-command-
         * no-op" race: the autostart display was actually alive but its
         * cold-boot init delayed past our PING window, so we spawned a
         * SECOND daemon. Both daemons answered subsequent PINGs; the
         * extra reply sat in the shell mailbox and the next
         * receive_wait() (inside readline) pulled it instead of the
         * typed-line reply — readline interpreted 4 bytes of display PID
         * as a length-prefixed line, memcpy'd garbage past the buffer,
         * and the shell silently dropped the user's first command.
         *
         * Fix: never spawn here. If no display answers in 2×2500 ms,
         * fall back to direct VGA I/O — single-daemon invariant is more
         * important than the "private display" affordance. */
        io_set_mode(IO_MODE_VGA);
    }

    /* Drain any stale messages left over from discovery — second
     * daemon's PING reply, retried broadcast echoes, etc. Without this
     * the FIRST readline pulls the stale reply and returns garbage. */
    ShellDrainStaleIpc();

    clear();
    println("BoxOS Shell v2.0");
    println("Type 'help' for commands, 'exit' to quit.");
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

/*
 * ShellDrainStaleIpc — drop everything sitting in the IPC mailbox.
 *
 * Used between user-visible operations so the next receive_wait inside
 * readline / executor never pulls a stale message left behind by:
 *   - a duplicate display-daemon PING reply during discovery
 *   - kernel-broadcast Touches the shell isn't subscribed to
 *
 * A spawned child sends the shell NOTHING on exit — its death is observed on
 * the process:died TouchRing, a separate ring — so this only clears stray
 * ResultRing traffic (display PING residue, stray broadcasts).
 *
 * Previously inlined at six call sites; centralised so a future change
 * (e.g. logging dropped traffic) edits one place.
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
        ShellDrainStaleIpc();

        int rc = LineEditRead(&g_editor, g_state.prompt, input, SHELL_LINE_MAX);

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
