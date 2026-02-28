#ifndef COMMANDS_H
#define COMMANDS_H

#include "box/defs.h"
#include "box/file.h"

// Command handler functions
// Returns: 0 on success, non-zero on error

// Basic commands (implemented in this stage)
int cmd_help(int argc, char *argv[]);
int cmd_exit(int argc, char *argv[]);
int cmd_clear(int argc, char *argv[]);

// File commands (next stage)
int cmd_create(int argc, char *argv[]);
int cmd_show(int argc, char *argv[]);
int cmd_files(int argc, char *argv[]);
int cmd_tag(int argc, char *argv[]);
int cmd_untag(int argc, char *argv[]);
int cmd_name(int argc, char *argv[]);
int cmd_trash(int argc, char *argv[]);
int cmd_erase(int argc, char *argv[]);

// Context commands (next stage)
int cmd_use(int argc, char *argv[]);

// System commands (next stage)
int cmd_me(int argc, char *argv[]);
int cmd_info(int argc, char *argv[]);
int cmd_say(int argc, char *argv[]);
int cmd_reboot(int argc, char *argv[]);
int cmd_bye(int argc, char *argv[]);

// Defragmentation commands
int cmd_defrag(int argc, char *argv[]);
int cmd_fsck(int argc, char *argv[]);

// IPC + multitasking demo
int cmd_ipc_test(int argc, char *argv[]);

// File lookup helper (implemented in cmd_files.c)
int find_file_by_name(const char *filename, uint32_t *file_ids, file_info_t *out_infos, size_t max);

#endif // COMMANDS_H
