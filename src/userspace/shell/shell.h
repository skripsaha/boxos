#ifndef SHELL_H
#define SHELL_H

#include "box/defs.h"

/* =========================================================================
 * BoxOS Shell Constants — no magic numbers in code
 * ========================================================================= */

#define SHELL_LINE_MAX          512     /* max input line length (matches LINE_CAPACITY) */
#define SHELL_MAX_ARGS          64      /* max tokens per command */
#define SHELL_PROMPT_MAX        128     /* max prompt string */
#define SHELL_MAX_CONTEXT_TAGS  16      /* max concurrent context tags */
#define SHELL_CONTEXT_TAG_LEN   32      /* max bytes per tag string */
#define SHELL_ARGS_BUF_MAX      240     /* IPC send() limit for args */
#define SHELL_ERROR_MAX         256     /* error message buffer */

#define SHELL_CHILD_POLL_MS     100     /* child wait poll interval */
#define SHELL_CHILD_DEAD_ITERS  10      /* polls before liveness check (1 sec) */
#define SHELL_EXIT_SENTINEL     0xFE    /* child exit IPC marker */

/* =========================================================================
 * Types
 * ========================================================================= */

typedef struct {
    char     prompt[SHELL_PROMPT_MAX];
    bool     running;
    uint32_t context_tag_count;
    char     context_tags[SHELL_MAX_CONTEXT_TAGS][SHELL_CONTEXT_TAG_LEN];
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

#endif /* SHELL_H */
