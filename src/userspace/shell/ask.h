#ifndef ASK_H
#define ASK_H

#include "box/defs.h"
#include "box/types.h"

/* How a question ended: answered, answered with nothing, or asked of no one
 * (the console's ear is gone). */
typedef enum {
    ASK_ANSWERED = 0,
    ASK_EMPTY,
    ASK_NO_ONE
} AskResult;

/* Print `prompt` and read one line, blanks trimmed off both ends. On
 * ASK_ANSWERED *text is a heap string the caller frees; otherwise NULL. */
AskResult AskLine(const char *prompt, char **text);

/* "<word> could be any of these:" with each file numbered and wearing its
 * tags, then "which one? (a number)". Asks again for a number that is not on
 * the list. */
AskResult AskWhich(const char *word, const file_id_t *ids, size_t count, file_id_t *chosen);

/* "all is the whole volume here: N files. sure? (yes or no)" */
AskResult AskSure(size_t count, bool *yes);

/* Say that `who` was not started because the question ended as `result`. */
void AskSayCancelled(const char *who, AskResult result);

#endif /* ASK_H */
