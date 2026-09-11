
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


const ShellCommand g_commands[] = {
    {"help",  cmd_help,  "help",       "Show available commands"},
    {"use",   cmd_use,   "use [tags]", "Set/clear the Use Context"},
    {"exit",  cmd_exit,  "exit",       "Exit shell (or Ctrl+Q)"},
    {"clear", cmd_clear, "clear",      "Clear screen"},
    {"said",  cmd_said,  "said [before]",
                                 "What this machine said (add a NAME to file it)"},
    {NULL, NULL, NULL, NULL}
};



static int RunExternal(const char *name, const char *line)
{
    uint32_t gen = 0;
    int pid = proc_exec_gen(line, NULL, &gen);
    if (pid <= 0) {
        return (pid == 0) ? -ERR_SPAWN_FAILED : pid;
    }

    int32_t child_exit = 0;
    int     gone_rc    = process_gone((uint32_t)pid, gen, &child_exit);
    if (gone_rc != 0) {
        kdbg_print("[shell] '%s' (pid %d gen %u): process.gone refused — rc=%d",
                   name, pid, gen, gone_rc);
    }

    return 0;
}


int ExecutorRun(ParsedCommand *cmd, const char *line)
{
    g_error[0] = '\0';

    if (!cmd || cmd->argc == 0 || !line) {
        memcpy(g_error, "No command", 11);
        return -1;
    }

    const char *name = cmd->argv[0];

    for (int i = 0; g_commands[i].name != NULL; i++) {
        if (strcmp(name, g_commands[i].name) == 0)
            return g_commands[i].handler(cmd->argc, cmd->argv);
    }

    int rc = RunExternal(name, line);
    if (rc == 0)
        return 0;

    error_t why = box_errno_of(rc);

    static const char kTail[] = ": is there and would not start";
    const size_t tail_len = sizeof(kTail) - 1;

    size_t name_len = strlen(name);
    if (why == ERR_FILE_NOT_FOUND) {
        if (name_len > SHELL_ERROR_MAX - 19) name_len = SHELL_ERROR_MAX - 19;
        memcpy(g_error, "Unknown command: ", 17);
        memcpy(g_error + 17, name, name_len);
        g_error[17 + name_len] = '\0';
    } else {
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