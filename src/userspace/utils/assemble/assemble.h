#ifndef ASSEMBLE_H
#define ASSEMBLE_H

#include "boxasm_elf.h"
#include "boxasm_source.h"

#include "box/types.h"

/* What one `assemble` command asks for, gathered before anything is built. */
typedef struct {
    char        SourceName[64];
    file_id_t   SourceId;
    file_id_t   Remake;        /* the program made again by its Recipe, 0 for a new build */
    char        RemakeName[64];
    char       *Home;          /* the Use Context the build began in */
    const char *RequestedOutput;
    const char *const *TagWords;
    size_t      TagWordCount;
    char        OutputName[64];
    char        Tags[2048];
    size_t      TagLength;
    char       *SourceBytes;
    size_t      SourceSize;
} AssembleRequest;

/* "assemble: <stage>: <detail>", the detail being the toolchain's own
 * sentence when it wrote one and the system's name for the error otherwise. */
void AssembleSayError(const char *Stage, int Rc, const char *Detail);

/* The volume's files as the toolchain asks for them (assemble_volume.c). */
BoxAsmFiles *AssembleVolume(void);
/* The Recipe of what the build has read: by assemble, in Context, the files
 * the volume handed it, the source first. The text is the caller's to free.
 * Returns 0 or a negative -error_t. */
int AssembleRecipe(const char *Context, char **Text, size_t *Length);
/* The Use Context as it is now, a comma list the caller frees: the home the
 * whole build looks in, taken once so a context said meanwhile moves none of
 * it. Returns 0 or a negative -error_t. */
int AssembleTakeContext(char **Out);

/* The built image put on the volume as the program the request names: the
 * one it replaces found by name and tags, asked about when more than one
 * could be meant — or, for a program made again by its Recipe and named by
 * no arrow, that program itself — written whole before the one it replaces
 * is let go. Says what happened; *Published is the program's file. Returns 0
 * or a negative -error_t. */
int AssemblePublish(const AssembleRequest *Request, const BoxAsmBlob *Image,
                    file_id_t *Published);

#endif
