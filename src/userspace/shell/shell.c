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
#include "box/memory.h"
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

    clear();
    println("BoxOS Shell v2.0");
    println("Type 'help' for commands, 'exit' to quit.");
    println("");
}

/* =========================================================================
 * Prompt management
 * ========================================================================= */

/* The whole context, comma-joined, into a buffer of its own size the caller
 * frees: the number of tags, 0 for none, negative when the kernel refused.
 * Asked for its size first and read again if it grew in between. */
int ShellContextWhole(char **out)
{
    *out = NULL;
    for (;;) {
        size_t needed = 0;
        int    rc     = use_get(NULL, 0, &needed);
        if (rc < 0 || needed <= 1) return rc < 0 ? rc : 0;

        char *list = malloc(needed);
        if (!list) return -1;
        size_t again = 0;
        int    tags  = use_get(list, needed, &again);
        if (tags >= 0 && again <= needed) {
            *out = list;
            return tags;
        }
        free(list);
        if (tags < 0) return tags;
    }
}

/* `n` in decimal at `out`; the number of bytes written. */
static size_t ShellDecimal(char *out, uint32_t n)
{
    char   digits[10];
    size_t count = 0;
    do {
        digits[count++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    for (size_t i = 0; i < count; i++)
        out[i] = digits[count - 1 - i];
    return count;
}

/* "] ~ " and its NUL, and " +4294967295" before them: what the prompt keeps
 * back from the tags. */
#define SHELL_PROMPT_TAIL  (5u + 12u)

/*
 * The prompt says what the kernel holds as the Use Context — "[code,cpp] ~ " —
 * read before every prompt rather than kept as a copy of what this shell
 * typed: the context is the machine's, so whoever said it last — this shell,
 * a nested one that has ended, a system program, a volume that came up with
 * its memory — is what the person sees. As many whole tags as the prompt has
 * room for are shown, never one torn mid-tag, and the rest are counted:
 * "[code,cpp +3]".
 */
static void ShellUpdatePrompt(void)
{
    char *whole = NULL;
    int   total = ShellContextWhole(&whole);
    if (total <= 0) {
        free(whole);
        memcpy(g_state.prompt, "~ ", 3);
        return;
    }

    char       *p     = g_state.prompt;
    size_t      used  = 0;
    int         shown = 0;
    const char *tag   = whole;
    p[used++] = '[';
    while (shown < total) {
        const char *comma = strchr(tag, ',');
        size_t      len   = comma ? (size_t)(comma - tag) : strlen(tag);
        size_t      cost  = len + (shown ? 1u : 0u);
        if (used + cost > SHELL_PROMPT_MAX - SHELL_PROMPT_TAIL) break;
        if (shown) p[used++] = ',';
        memcpy(p + used, tag, len);
        used += len;
        shown++;
        if (!comma) break;
        tag = comma + 1;
    }
    free(whole);
    if (shown < total) {
        if (shown) p[used++] = ' ';
        p[used++] = '+';
        used += ShellDecimal(p + used, (uint32_t)(total - shown));
    }
    memcpy(p + used, "] ~ ", 5);
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

int ShellRunLine(const char *input)
{
    ParsedCommand cmd;
    if (ParserParse(input, &cmd) != 0) {
        println("Error: no memory to read the command");
        return -1;
    }
    int rc = cmd.argc > 0 ? ExecutorRun(&cmd, input) : 0;
    if (rc != 0) {
        const char *err = ExecutorGetError();
        if (err && err[0] != '\0') {
            printf("%colorError: %s%color\n",
                   COLOR_RED, err, COLOR_DEFAULT);
        }
    }
    ParserFree(&cmd);
    return rc;
}

void ShellMainLoop(void)
{
    while (g_state.running) {
        ShellUpdatePrompt();
        print(g_state.prompt);
        io_flush();

        /* The line grows as it is typed: no ceiling but memory. */
        char *input = NULL;
        int   len   = readline_grow(&input);
        if (len > 0)
            ShellRunLine(input);
        free(input);
    }
}

void ShellStop(void)
{
    g_state.running = false;
}

bool ShellRunning(void)
{
    return g_state.running;
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
