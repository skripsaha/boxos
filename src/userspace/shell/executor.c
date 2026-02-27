#include "executor.h"
#include "commands/commands.h"
#include "box/string.h"
#include "box/system.h"

static char g_error_message[128];

static const shell_command_t g_commands[] = {
    { "help", cmd_help, "help", "Show available commands" },
    { "exit", cmd_exit, "exit", "Exit shell" },
    { "create", cmd_create, "create <file> [tags]", "Create file" },
    { "show", cmd_show, "show <file>", "Show file content" },
    { "files", cmd_files, "files [tags]", "List files" },
    { "tag", cmd_tag, "tag <file> <key:val>", "Add tag" },
    { "untag", cmd_untag, "untag <file> <key>", "Remove tag" },
    { "name", cmd_name, "name <old> <new>", "Rename file" },
    { "trash", cmd_trash, "trash <file>", "Move to trash" },
    { "erase", cmd_erase, "erase <file|trashed>", "Delete file(s)" },
    { "use", cmd_use, "use [tags]", "Set/clear context" },
    { "me", cmd_me, "me", "System info" },
    { "info", cmd_info, "info [object]", "Object info" },
    { "say", cmd_say, "say <text...>", "Print text" },
    { "reboot", cmd_reboot, "reboot", "Reboot system" },
    { "bye", cmd_bye, "bye", "Shutdown system" },
    { "defrag", cmd_defrag, "defrag <file>", "Defragment file" },
    { "fsck", cmd_fsck, "fsck", "Check fragmentation" },
    { NULL, NULL, NULL, NULL }
};

int executor_run(parsed_command_t* cmd) {
    g_error_message[0] = '\0';

    if (!cmd || cmd->argc == 0) {
        memcpy(g_error_message, "No command", 11);
        return -1;
    }

    const char* command_name = cmd->argv[0];

    for (int i = 0; g_commands[i].name != NULL; i++) {
        if (strcmp(command_name, g_commands[i].name) == 0) {
            return g_commands[i].handler(cmd->argc, cmd->argv);
        }
    }

    // Try to launch as executable from TagFS
    int pid = proc_exec(command_name);
    if (pid > 0) {
        return 0;
    }

    memcpy(g_error_message, "Unknown command: ", 17);
    size_t cmd_len = strlen(command_name);
    if (cmd_len > 100) cmd_len = 100;
    memcpy(g_error_message + 17, command_name, cmd_len);
    g_error_message[17 + cmd_len] = '\0';

    return -1;
}

const char* executor_get_error(void) {
    return g_error_message;
}
