#ifndef INSIDE_SHOW_H
#define INSIDE_SHOW_H

/*
 * inside — what a program file is, said for a person: a box's parts in the
 * order of its table, each one's size, what it is and what it says, its flags,
 * its hash held; the seal; and whether the box is whole. A file that is no box
 * is said to be what it is.
 *
 * One source for the utility in BoxOS (utils/inside.c) and the tool on the
 * build machine (tools/inside.c): the file is read and the words are said
 * through what the caller hands over, and the SHA-256 is the caller's, so
 * nothing here reaches a volume, a terminal or a library of its own beyond
 * memory and a few string functions. The box is read through the one reader
 * (box_reader.h), as the kernel and BoxLib read it.
 *
 * Integer types come from the includer, as box_reader.h's do.
 */

#include "box_reader.h"

typedef enum {
    INSIDE_WHOLE = 0,   /* a box whose seal, parts and gaps all hold */
    INSIDE_DAMAGED,     /* a box something of which does not hold — said where */
    INSIDE_NO_BOX,      /* an ELF image, or no program at all */
    INSIDE_UNREAD,      /* the file could not be read, or there was no memory to */
} InsideVerdict;

typedef struct {
    const char       *Name;      /* what the file is called in what is said */
    uint64_t          Size;
    /* `Count` bytes from `Offset` into `Into`, whole — or false. */
    bool            (*Read)(void *Context, uint64_t Offset, void *Into, uint64_t Count);
    /* `Length` bytes of what is said, a line ended by '\n'. */
    void            (*Say)(void *Context, const char *Text, uint64_t Length);
    void             *Context;
    const BoxHasher  *Hasher;
} InsideFile;

/* Everything inside `file`, said; what it comes to. */
InsideVerdict InsideShow(const InsideFile *file);

#endif /* INSIDE_SHOW_H */
