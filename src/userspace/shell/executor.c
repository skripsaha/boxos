#include "executor.h"
#include "commands/commands.h"
#include "box/string.h"
#include "box/system.h"
#include "box/ipc.h"
#include "box/result.h"
#include "box/io.h"

static char g_error_message[128];

static const shell_command_t g_commands[] = {
    {"help", cmd_help, "help", "Show available commands"},
    {"exit", cmd_exit, "exit", "Exit shell"},
    {"clear", cmd_clear, "clear", "Clear the screen"},
    {"create", cmd_create, "create <file> [tags]", "Create file"},
    {"show", cmd_show, "show <file>", "Show file content"},
    {"files", cmd_files, "files [tags]", "List files"},
    {"tag", cmd_tag, "tag <file> <key:val>", "Add tag"},
    {"untag", cmd_untag, "untag <file> <key>", "Remove tag"},
    {"name", cmd_name, "name <old> <new>", "Rename file"},
    {"trash", cmd_trash, "trash <file>", "Move to trash"},
    {"erase", cmd_erase, "erase <file|trashed>", "Delete file(s)"},
    {"use", cmd_use, "use [tags]", "Set/clear context"},
    {"me", cmd_me, "me", "System info"},
    {"info", cmd_info, "info [object]", "Object info"},
    {"say", cmd_say, "say <text...>", "Print text"},
    {"reboot", cmd_reboot, "reboot", "Reboot system"},
    {"bye", cmd_bye, "bye", "Shutdown system"},
    {"defrag", cmd_defrag, "defrag <file>", "Defragment file"},
    {"fsck", cmd_fsck, "fsck", "Check fragmentation"},
    {"ipc-test", cmd_ipc_test, "ipc-test", "IPC + multitasking demo"},
    {NULL, NULL, NULL, NULL}};

int executor_run(parsed_command_t *cmd)
{
    g_error_message[0] = '\0';

    // Drain stale IPC from previous commands BEFORE launching anything.
    // Must be here (not after proc_exec) because proc_exec may schedule
    // the child first — its IPC messages get stashed by result_pop_non_ipc
    // during proc_exec's result_wait, and a post-exec drain would discard them.
    {
        result_entry_t stale;
        while (receive(&stale)) { /* discard */ }
    }

    if (!cmd || cmd->argc == 0)
    {
        memcpy(g_error_message, "No command", 11);
        return -1;
    }

    const char *command_name = cmd->argv[0];

    for (int i = 0; g_commands[i].name != NULL; i++)
    {
        if (strcmp(command_name, g_commands[i].name) == 0)
        {
            return g_commands[i].handler(cmd->argc, cmd->argv);
        }
    }

    // Try to launch as executable from TagFS
    int pid = proc_exec(command_name);
    if (pid > 0)
    {
        // No drain here! Messages stashed during proc_exec are valid current output.
        // receive_wait → result_pop_ipc checks the stash first.
        result_entry_t entry;
        int idle_count = 0;
        int got_output = 0;
        while (idle_count < (got_output ? 3 : 2))
        {
            // After first output: short timeout — messages arrive fast from
            // stash/ring, we just detect "no more" quickly (3×30ms = 90ms).
            // Before first output: longer timeout — give child time to start
            // and send first message (2×100ms = 200ms).
            if (receive_wait(&entry, got_output ? 30 : 100))
            {
                char buf[245];
                uint16_t len = entry.size;
                if (len > 244)
                    len = 244;
                memcpy(buf, entry.payload, len);
                buf[len] = '\0';
                print(buf);
                got_output = 1;
                idle_count = 0;
            }
            else
            {
                idle_count++;
            }
        }
        return 0;
    }

    memcpy(g_error_message, "Unknown command: ", 17);
    size_t cmd_len = strlen(command_name);
    if (cmd_len > 100)
        cmd_len = 100;
    memcpy(g_error_message + 17, command_name, cmd_len);
    g_error_message[17 + cmd_len] = '\0';

    return -1;
}

const char *executor_get_error(void)
{
    return g_error_message;
}
