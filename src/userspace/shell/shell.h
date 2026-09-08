#ifndef SHELL_H
#define SHELL_H

#include "box/defs.h"

/* =========================================================================
 * BoxOS Shell Constants — no magic numbers in code
 * ========================================================================= */

#define SHELL_LINE_MAX          512     /* max input line length (matches LINE_CAPACITY) */
#define SHELL_MAX_ARGS          64      /* max tokens per command */
#define SHELL_PROMPT_MAX        128     /* max prompt string */
#define SHELL_ERROR_MAX         256     /* error message buffer */

/* =========================================================================
 * Types
 * ========================================================================= */

typedef struct {
    char     prompt[SHELL_PROMPT_MAX];
    bool     running;
} ShellState;

typedef struct {
    int   argc;
    char *argv[SHELL_MAX_ARGS];
    char  storage[SHELL_LINE_MAX];
} ParsedCommand;

typedef int (*CommandHandler)(int argc, char *argv[]);

typedef struct {
    const char     *name;
    CommandHandler  handler;
    const char     *usage;
    const char     *description;
} ShellCommand;

/* =========================================================================
 * Public API
 * ========================================================================= */

void        ShellInit(void);
void        ShellMainLoop(void);
void        ShellStop(void);
ShellState *ShellGetState(void);
void        ShellUpdatePrompt(void);

/* Drop everything sitting in the IPC mailbox. Use between operations
 * that could otherwise consume a leftover Result (a late display PING
 * reply, stray kernel Touch). See shell.c for the
 * historical "first-command-no-op" race this guards against. */
void        ShellDrainStaleIpc(void);

#endif /* SHELL_H */
