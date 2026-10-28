#ifndef ASSEMBLE_H
#define ASSEMBLE_H

#include "boxasm_elf.h"

#include "box/types.h"

/* What one `assemble` command asks for, gathered before anything is built. */
typedef struct {
    const char *SourceName;
    file_id_t   SourceId;
    const char *RequestedOutput;
    int         TagWordStart;
    char        OutputName[64];
    char        Tags[2048];
    size_t      TagLength;
    int         AddedApp;
    uint8_t    *SourceBytes;
    uint64_t    SourceSize;
} AssembleRequest;

/* "assemble: <stage>: <detail>", the detail being the toolchain's own
 * sentence when it wrote one and the system's name for the error otherwise. */
void AssembleSayError(const char *Stage, int Rc, const char *Detail);
const char *AssembleDescribeError(int Rc);

/* Whether the comma list wears a tag with this key, with or without a value. */
int AssembleTagsHaveKey(const char *Tags, const char *Key);

/* The built image put on the volume as the program the request names: the
 * one it replaces found by name and tags, asked about when more than one
 * could be meant, written whole before the one it replaces is let go. Says
 * what happened. Returns 0 or a negative -error_t. */
int AssemblePublish(const AssembleRequest *Request, const BoxAsmBlob *Image);

#endif
