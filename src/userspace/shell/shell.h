#ifndef SHELL_H
#define SHELL_H

#include "box/defs.h"


#define SHELL_LINE_MAX          512
#define SHELL_MAX_ARGS          64
#define SHELL_PROMPT_MAX        128
#define SHELL_ERROR_MAX         256


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


void        ShellInit(void);
void        ShellMainLoop(void);
void        ShellStop(void);
ShellState *ShellGetState(void);
void        ShellUpdatePrompt(void);

void        ShellDrainStaleIpc(void);

#endif