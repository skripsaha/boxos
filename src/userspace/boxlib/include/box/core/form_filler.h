#ifndef BOX_CORE_FORM_FILLER_H
#define BOX_CORE_FORM_FILLER_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/core/form_internal.h"

/*
 * The filler's state, shared by form_fill.c (the calls) and
 * form_fill_assign.c (the sorting). A line is cut into items; the sort
 * runs again from the start after every answer, over items that remember
 * what was looked up and what was answered, so asking never needs a place to
 * resume from.
 */

enum {
    FORM_ROLE_WORD = 0,
    FORM_ROLE_THEM,
    FORM_ROLE_IT,
    FORM_ROLE_ALL
};

enum {
    FORM_PENDING_NONE = 0,
    FORM_PENDING_WHICH,
    FORM_PENDING_SURE,
    FORM_PENDING_FIELD,
    FORM_PENDING_ARROW
};

typedef struct {
    FormToken  Token;
    uint8_t    Role;
    uint8_t    Resolved;       /* Find / Them / All answered into Ids */
    uint8_t    Bounded;        /* ALL: the Use Context bounds it */
    uint8_t    Sure;           /* ALL: the whole volume was agreed to */
    uint8_t    Taken;          /* by a field, in the sort now running */
    uint8_t    Dropped;        /* let go with an answer */
    uint8_t    Everywhere;
    uint8_t    Bound;          /* 0: from the line; else the field it was answered for, plus 1 */
    file_id_t *Ids;
    size_t     IdCount;
} FormItem;

struct BoxFormFiller {
    const BoxForm  *Form;
    BoxFormLookup  *Lookup;
    char           *Line;
    char           *Program;
    FormItem       *Items;
    size_t          ItemCount;
    size_t          ItemCapacity;
    uint8_t         HasThing;

    char           *ArrowName;       /* the word after `->`, or NULL */
    uint32_t        ArrowStart;      /* where `->` began in Line */
    uint32_t        ArrowEnd;        /* after the target, and the (file) if any */
    uint8_t         ArrowEverywhere;
    uint8_t         ArrowToInto;     /* the sort gave the target to an `into` field */
    file_id_t      *ArrowIds;
    size_t          ArrowCount;
    uint8_t         ArrowResolved;
    file_id_t       ArrowChosen;

    FormValues      Values;
    file_id_t       Screen;
    const char     *Create;
    const char     *Reasons[BOX_FORM_FIELDS];

    char           *Sentence;
    BoxFormQuestion Question;
    uint8_t         Pending;
    size_t          PendingItem;
    size_t          Suspect;         /* the item named as a file that is not here, plus 1 */
    uint32_t        PendingField;
    uint8_t         Finished;
    uint8_t         Refused;
};

/* A refusal whose sentence is A, B and C joined; the step to return, or
 * -ERR_NO_MEMORY. */
int form_fill_refuse(BoxFormFiller *filler, const char *a, const char *b, const char *c);

/* The line from offset `from`, as typed, without the arrow; heap, NULL when
 * there is no memory. */
char *form_fill_line_from(const BoxFormFiller *filler, size_t from, size_t *length);

/* One sort of the items into the fields: the next step, or a negative error. */
int form_fill_assign(BoxFormFiller *filler);

/* The items of tokens, appended; `Bound` 0 for the line. Words that are the
 * shell's keywords are read as such when `keywords`. The tokens' texts move
 * into the items. */
int form_fill_add_items(BoxFormFiller *filler, FormTokens *tokens, uint8_t bound,
                        bool keywords, size_t skip_from, size_t skip_count);

#ifdef __cplusplus
}
#endif

#endif /* BOX_CORE_FORM_FILLER_H */
