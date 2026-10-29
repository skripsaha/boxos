/*
 * executor.c — BoxOS shell command executor
 *
 * Built-in commands are looked up in g_commands[].
 * External commands are programs found by name, whose Form the typed line is
 * sorted into (program.c, fill.c).
 */

#include "executor.h"
#include "shell.h"
#include "commands/commands.h"
#include "parser.h"
#include "program.h"
#include "fill.h"
#include "itinerary.h"
#include "suggest.h"
#include "box/string.h"
#include "box/error.h"
#include "box/core/result.h"
#include "box/print.h"

static char        g_error[SHELL_ERROR_MAX];
static const char *g_line = "";

/* =========================================================================
 * Command table — all built-in commands registered here
 * ========================================================================= */

const ShellCommand g_commands[] = {
    {"help",  cmd_help,  "help [NAME]", "Show available commands, or what a program takes"},
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
    {"run",   cmd_run,   "run THING",  "Start a program given as a file, or `it`"},
    {NULL, NULL, NULL, NULL}
};

/* =========================================================================
 * External command execution
 * ========================================================================= */

/* The programs the name means are found the way the kernel would find them
 * and the person is asked which when there are several. The words after the
 * name are sorted into the program's Form (fill.c), which starts it and waits
 * for its end — or into an Itinerary's, whose steps then run (itinerary.c).
 * 0 when it went through, 1 when it did not and why was said; else the
 * reason no program was found. */
static int RunExternal(const char *name, const char *line)
{
    file_id_t program;
    int picked = ProgramPick(name, &program);
    if (picked == PROGRAM_SAID)
        return 1;
    if (picked < 0)
        return picked;
    const char *rest = ParserRest(line, 1);
    bool went = ProgramIsItinerary(program) ? ItineraryRun(program, name, rest)
                                            : FillAndRun(program, name, rest);
    return went ? 0 : 1;
}

/* The names close to the one typed, on a line of their own after the message;
 * nothing when none is close or they do not fit. */
static void AppendSuggestion(const char *name)
{
    char suggestion[SHELL_ERROR_MAX];
    if (!SuggestFor(name, suggestion, sizeof(suggestion))) return;

    size_t used = strlen(g_error);
    size_t length = strlen(suggestion);
    if (used + 1 + length >= SHELL_ERROR_MAX) return;
    g_error[used] = '\n';
    memcpy(g_error + used + 1, suggestion, length + 1);
}

/* =========================================================================
 * Public API
 * ========================================================================= */

int ExecutorRun(ParsedCommand *cmd, const char *line)
{
    g_error[0] = '\0';
    g_line = line ? line : "";

    if (!cmd || cmd->argc == 0 || !line) {
        memcpy(g_error, "No command", 11);
        return -1;
    }

    const char *name = cmd->argv[0];

    /* Check built-in commands. What a command said is said: a line run inside
     * it (an Itinerary's step) leaves no sentence here to be said twice. */
    for (int i = 0; g_commands[i].name != NULL; i++) {
        if (strcmp(name, g_commands[i].name) == 0) {
            int rc = g_commands[i].handler(cmd->argc, cmd->argv);
            g_error[0] = '\0';
            return rc;
        }
    }

    /* Try external utility */
    int rc = RunExternal(name, line);
    if (rc >= 0) {
        g_error[0] = '\0';
        return rc;
    }

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
        AppendSuggestion(name);
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

const char *ExecutorLine(void)
{
    return g_line;
}

const char *ExecutorGetError(void)
{
    return g_error;
}
