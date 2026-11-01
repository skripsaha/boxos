#ifndef BOXASM_SOURCE_H
#define BOXASM_SOURCE_H

#include "boxasm_elf.h"
#include "boxasm_support.h"

/* Where the files a source names are found — the volume inside BoxOS, a set
 * of files on the host in the stand — asked three things. Each answers 0 or
 * more, or a negative -error_t. */
typedef struct BoxAsmFiles BoxAsmFiles;
struct BoxAsmFiles {
    /* Every file called Name that wears every tag of Tags (a comma list, ""
     * for none), wherever it is: their ids into *Ids, which the caller frees,
     * and how many. */
    int (*Find)(BoxAsmFiles *Files, const char *Name, const char *Tags, uint64_t **Ids);
    /* A file's tags, a comma list the caller frees. */
    int (*Tags)(BoxAsmFiles *Files, uint64_t Id, char **Out);
    /* A file's bytes with a NUL after them, which the caller frees;
     * -ERR_OUT_OF_RANGE when it holds more than Limit. */
    int (*Read)(BoxAsmFiles *Files, uint64_t Id, size_t Limit, char **Out, size_t *Size);
};

/* What to assemble: the source's text and name and, when it is a file of a
 * volume, which file, the tags it was found among — its home, "" for the
 * whole volume — and the volume its %includes are found on (NULL: the
 * source includes nothing); and where what it says goes (NULL: nowhere). */
typedef struct {
    const char   *Text;
    size_t        Size;
    const char   *Name;
    uint64_t      Id;
    const char   *Home;
    BoxAsmFiles  *Files;
    BoxAsmBuffer *Said;   /* what %warning says, a line each: file:line: warning: text */
} BoxAsmSource;

/* Assemble BoxASM source into an x86-64 ELF relocatable object: the program
 * must define `main` in .text and declare it global. Diagnostics are written
 * into Error as "file:line:column: what is wrong", the line itself, a caret
 * under the place, and the macro calls and %includes it was reached
 * through. The object is heap-owned by Out->Data. */
int BoxAsmSourceAssemble(const BoxAsmSource *Source, BoxAsmBlob *Out, char *Error,
                         size_t ErrorCapacity);

/* ---- Finding a file by its name and tags ---------------------------------- */

/* One tag of a comma list, read as the volume reads it: the blanks around it
 * dropped, split at its first ':', an empty value the same as none. */
typedef struct {
    const char *Key;
    size_t      KeyLength;
    const char *Value;
    size_t      ValueLength;
} BoxAsmTag;

/* The next tag of List from *At on; 0 at its end. */
int BoxAsmNextTag(const char *List, size_t *At, BoxAsmTag *Out);
/* Whether List holds Tag itself, or with KeyOnly any tag of Tag's key. */
int BoxAsmTagsHold(const char *List, const BoxAsmTag *Tag, int KeyOnly);

/* What the system calls an error, in its own words. */
const char *BoxAsmErrorText(int Rc);

/* A file asked for by name: the meaning it must wear, and where it is looked
 * for — among the tags of a home (the context the work began in; "" for the
 * whole volume) or, Everywhere, past it. */
typedef struct {
    const char *Name;
    const char *Tags;          /* a comma list, "" for none */
    const char *Home;
    uint8_t     Everywhere;
    const char *Directive;     /* asked by a line, %include or incbin: what to write is
                                  said as such a line; NULL when the command asks */
} BoxAsmLookup;

/* The one file a lookup means: 0 and its id. Otherwise the refusal said in
 * Summary, one line, and Details, lines that follow it: -ERR_INVALID_STATE
 * for several, each with a tag that picks it; -ERR_FILE_NOT_FOUND for none —
 * the meaning that did not fit those there are, or where the ones outside
 * the home are and what reaches them. Another negative -error_t is the
 * volume's, or no memory, with nothing said. */
int BoxAsmLocate(BoxAsmFiles *Files, const BoxAsmLookup *Lookup, uint64_t *Id,
                 BoxAsmBuffer *Summary, BoxAsmBuffer *Details);

#endif
