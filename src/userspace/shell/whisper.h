#ifndef WHISPER_H
#define WHISPER_H

#include "box/print.h"
#include "box/form.h"
#include "view.h"

typedef struct WhisperRecent WhisperRecent;

/* The shell's Whisper for the line at its prompt (box/print.h): what the line
 * could go on to be, from the person's own history and from what the command
 * typed takes, and what that command still needs. One for each line: begun
 * before it is read, ended after. */
typedef struct {
    LineWhisper           Line;          /* first: what the editor is handed */
    ShellView             View;
    char                 *Context;       /* the Use Context as the line began, NULL for none */
    char                 *FormOf;        /* the command whose Form is held, NULL for none */
    BoxForm               Form;
    bool                  FormHeld;      /* Form is FormOf's, read */
    WhisperRecent        *Recent;        /* every word of the history, as typed */
    const WhisperRecent **RecentOrder;   /* each word once, in byte order */
    size_t                RecentCount;
    bool                  RecentRead;
} ShellWhisper;

void WhisperBegin(ShellWhisper *whisper);
void WhisperEnd(ShellWhisper *whisper);

#endif /* WHISPER_H */
