#ifndef SHELL_H
#define SHELL_H

#include "box/defs.h"

/* =========================================================================
 * BoxOS Shell Constants — no magic numbers in code
 * ========================================================================= */

#define SHELL_PROMPT_MAX        128     /* max prompt string */
#define SHELL_ERROR_MAX         256     /* error message buffer */

/* =========================================================================
 * Types
 * ========================================================================= */

typedef struct {
    char     prompt[SHELL_PROMPT_MAX];
    bool     running;
} ShellState;

/* A typed line cut into words: as many words as the line has, each a string
 * in `storage`, a copy of the line of its own length. Both arrays are the
 * parser's, given back with ParserFree. */
typedef struct {
    int    argc;
    char **argv;
    char  *storage;
} ParsedCommand;

typedef int (*CommandHandler)(int argc, char *argv[]);

/* What a built-in command's words are — a built-in has no Form, and this is
 * the part of one the Whisper needs. */
typedef enum {
    SHELL_TAKES_NOTHING = 0,
    SHELL_TAKES_COMMAND,       /* its first word is a command's name */
    SHELL_TAKES_PROGRAM,       /* its first word is a program's file */
    SHELL_TAKES_TAGS           /* its words are tags, + or - before each when changing */
} ShellTakes;

typedef struct {
    const char     *name;
    CommandHandler  handler;
    const char     *usage;
    const char     *description;
    ShellTakes      takes;
} ShellCommand;

/* =========================================================================
 * Public API
 * ========================================================================= */

void        ShellInit(void);
void        ShellMainLoop(void);
void        ShellStop(void);
/* Whether the shell goes on: false once `exit` was said. */
bool        ShellRunning(void);
/* One line, whatever its length, run as if typed: the cut words for a
 * built-in, the line itself for a program. 0 when it went through; anything
 * else when it did not, and why was said. */
int         ShellRunLine(const char *line);
ShellState *ShellGetState(void);
/* The Use Context as the kernel holds it, comma-joined, into a buffer of its
 * own size the caller frees: the number of tags, 0 for none (*out NULL),
 * negative when the kernel refused. */
int         ShellContextWhole(char **out);

/* Drop everything sitting in the IPC mailbox. Use between operations
 * that could otherwise consume a leftover Result (a late display PING
 * reply, stray kernel Touch). See shell.c for the
 * historical "first-command-no-op" race this guards against. */
void        ShellDrainStaleIpc(void);

#endif /* SHELL_H */
