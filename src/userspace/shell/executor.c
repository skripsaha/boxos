/*
 * executor.c — BoxOS shell command executor
 *
 * Built-in commands are looked up in g_commands[].
 * External commands are spawned via proc_exec_gen() with the typed line as
 * their Luggage; the child's end is waited for as a STATE (process_gone).
 */

#include "executor.h"
#include "shell.h"
#include "commands/commands.h"
#include "box/string.h"
#include "box/system.h"
#include "box/touch.h"
#include "box/debug.h"
#include "box/error.h"
#include "box/core/result.h"
#include "box/print.h"

static char g_error[SHELL_ERROR_MAX];

/* =========================================================================
 * Command table — all built-in commands registered here
 * ========================================================================= */

const ShellCommand g_commands[] = {
    {"help",  cmd_help,  "help",       "Show available commands"},
    {"use",   cmd_use,   "use [tags]", "Set/clear the Use Context"},
    {"exit",  cmd_exit,  "exit",       "Exit shell (or Ctrl+Q)"},
    {"clear", cmd_clear, "clear",      "Clear screen"},
    /* Built in, and that is the whole point of it: the board failure being
     * chased is one where the volume mounts and reports its files while the
     * shell can find none of them, so `logsave` and `lastsaid` — both ELF
     * files ON that volume — come back Unknown at the exact moment the log is
     * worth having. This needs no lookup and no spawn. Both of those tools
     * remain; this is the copy that cannot be taken away. */
    {"said",  cmd_said,  "said [before]",
                                 "What this machine said (add a NAME to file it)"},
    {NULL, NULL, NULL, NULL}
};

/* =========================================================================
 * External command execution
 * ========================================================================= */

/* The shell no longer subscribes to process:died at all.
 *
 * It used to claim that multicast and read a child's exit off the TouchRing.
 * The claim carried two costs and one defect. It had to be armed BEFORE the
 * spawn and drained after, because a claim is a subscription to EDGES and an
 * edge that arrives before you listen is simply not there. And a multicast is
 * best-effort: TouchPublish may drop, and a dropped death left the shell
 * waiting for one for the rest of the boot — measured on a wedged UEFI 16c
 * machine, fifteen cores parked and the shell spinning in touch_wait forever,
 * with no prompt ever again.
 *
 * process_gone asks the same question as a STATE instead, so there is no
 * before-and-after to get wrong, nothing to pre-arm, nothing to drain, and
 * nothing that can be dropped. */

/* The line goes to the child whole, as typed: it is the child's Luggage,
 * in its cabin before its first instruction. Nothing is sent after the
 * spawn, so there is no message to be late, no buffer to fill, no word to
 * cut short — the 240-byte IPC buffer, the 64-byte words and the child's
 * one-second wait for them are all gone with it. */
static int RunExternal(const char *name, const char *line)
{
    uint32_t gen = 0;
    int pid = proc_exec_gen(line, NULL, &gen);
    if (pid <= 0) {
        /* Carried up, not flattened. "There is no such program" and "the
         * program is there and would not start" are different facts, and the
         * caller printed the same sentence for both. */
        return (pid == 0) ? -ERR_SPAWN_FAILED : pid;
    }

    /* Wait until THIS child is gone — one park, no clock, nothing to poll.
     *
     * The kernel answers from the child's own record: already finished is
     * reported at once, still running is parked on, and the single place a
     * life ends pays every waiter it owes. Because the question is about a
     * state rather than an event, there is no window between asking and being
     * registered — the shape of failure that used to hang this shell (a
     * dropped process:died) has no room left to happen.
     *
     * A missing answer would now be a kernel defect that Nightwatch can prove,
     * which is the other half of the repair: the old wait spun in userspace,
     * so the shell stayed PROC_WORKING and that spin also kept every core from
     * ever looking idle — the oracle was blinded twice over, which is why this
     * took three sessions to name. A parked waiter is visible. */
    int32_t child_exit = 0;
    int     gone_rc    = process_gone((uint32_t)pid, gen, &child_exit);
    if (gone_rc != 0) {
        /* The only honest failure left: the kernel says this incarnation was
         * never issued, i.e. it vanished between spawn and ask. Say it rather
         * than return as if the program had run. */
        kdbg_print("[shell] '%s' (pid %d gen %u): process.gone refused — rc=%d",
                   name, pid, gen, gone_rc);
    }

    return 0;
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int ExecutorRun(ParsedCommand *cmd, const char *line)
{
    g_error[0] = '\0';

    if (!cmd || cmd->argc == 0 || !line) {
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
    int rc = RunExternal(name, line);
    if (rc == 0)
        return 0;

    /*
     * ‼ SAY WHICH OF THE TWO THINGS HAPPENED.
     *
     * "Unknown command" used to be printed for every failure to start a
     * program, including the one that matters most on a machine you can pull
     * the disk out of: the program IS on the volume, and the volume is not
     * there any more. Measured on a live board — the stick was out, the user
     * typed a command that exists, and the machine told them the command does
     * not exist. The kernel had just printed the truth two lines above.
     */
    error_t why = box_errno_of(rc);

    /* ASCII only, and measured rather than counted by hand: the sentence that
     * used to sit here carried an em dash, three bytes for one character, and
     * the length beside it was the count of characters. It copied one byte
     * short and printed half a word. On a machine read off a photograph, half
     * a sentence is worse than none. */
    static const char kTail[] = ": is there and would not start";
    const size_t tail_len = sizeof(kTail) - 1;

    size_t name_len = strlen(name);
    if (why == ERR_FILE_NOT_FOUND) {
        if (name_len > SHELL_ERROR_MAX - 19) name_len = SHELL_ERROR_MAX - 19;
        memcpy(g_error, "Unknown command: ", 17);
        memcpy(g_error + 17, name, name_len);
        g_error[17 + name_len] = '\0';
    } else {
        /* The name is real; starting it is what failed. */
        if (name_len > SHELL_ERROR_MAX - tail_len - 2)
            name_len = SHELL_ERROR_MAX - tail_len - 2;
        memcpy(g_error, name, name_len);
        memcpy(g_error + name_len, kTail, tail_len);
        g_error[name_len + tail_len] = '\0';
    }
    return -1;
}

const char *ExecutorGetError(void)
{
    return g_error;
}
