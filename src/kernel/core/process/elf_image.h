#ifndef ELF_IMAGE_H
#define ELF_IMAGE_H

/*
 * An ELF64 program read for loading, and nothing in it taken on trust.
 *
 * The file is untrusted: it comes off a volume anyone may have written, or out
 * of a cabin that hands it to the spawn. So it is read whole before a single
 * page is given to it, and refused for the first thing that is not so: every
 * offset and size held to the file by subtraction, never by an addition that
 * could wrap; every segment held to the zone of the address space a program's
 * image may take; the segments in ascending order and never two on one page;
 * no segment writable and executable at once; the entry inside code the file
 * supplies. What passes is what the loader maps, and the loader reads it
 * through ElfImageNextSegment — the same decoding the check used — so it
 * cannot be handed a segment the check did not see.
 *
 * Pure: no allocation, no global, nothing of the kernel but its integer types.
 * The host stand (tools/elf_stand.c) builds elf_image.c as the kernel does and
 * holds every refusal to its exact verdict. Integer types come from the
 * includer, as everywhere a file is read by the kernel and by a host tool:
 *   Kernel:     #include "ktypes.h"
 *   Host stand: #include <stdint.h> and <stdbool.h>
 */

typedef enum {
    ELF_IMAGE_OK = 0,
    ELF_IMAGE_NOT_ELF,
    ELF_IMAGE_TRUNCATED,
    ELF_IMAGE_NOT_64_BIT,
    ELF_IMAGE_NOT_LITTLE_ENDIAN,
    ELF_IMAGE_NOT_EXECUTABLE,
    ELF_IMAGE_NOT_X86_64,
    ELF_IMAGE_HEADER_SIZE,
    ELF_IMAGE_NO_HEADERS,
    ELF_IMAGE_HEADER_COUNT_ELSEWHERE,
    ELF_IMAGE_HEADERS_OUTSIDE_FILE,
    ELF_IMAGE_SEGMENT_FILE_OVER_MEMORY,
    ELF_IMAGE_SEGMENT_OUTSIDE_FILE,
    ELF_IMAGE_SEGMENT_OUTSIDE_ZONE,
    ELF_IMAGE_SEGMENT_OUT_OF_ORDER,
    ELF_IMAGE_SEGMENT_WRITABLE_EXECUTABLE,
    ELF_IMAGE_STACK_EXECUTABLE,
    ELF_IMAGE_INTERPRETER,
    ELF_IMAGE_NOTHING_TO_LOAD,
    ELF_IMAGE_ENTRY_OUTSIDE_CODE,
    ELF_IMAGE_VERDICT_COUNT
} ElfImageVerdict;

/* What Header holds when a verdict is about the file as a whole. It is
 * PN_XNUM, which an accepted table never reaches as an index. */
#define ELF_IMAGE_NO_HEADER 0xFFFFu

typedef struct {
    const uint8_t *File;
    uint64_t       FileBytes;
    uint64_t       Entry;
    uint64_t       HeadersAt;    /* where the program header table starts */
    uint16_t       HeaderCount;
    uint16_t       Header;       /* the program header a refusal is about */
} ElfImage;

/* One loadable segment: `FileBytes` bytes from `FileOffset` of the file put at
 * `Virt`, and zeros after them up to `MemoryBytes`. */
typedef struct {
    uint64_t Virt;
    uint64_t FileOffset;
    uint64_t FileBytes;
    uint64_t MemoryBytes;
    bool     Writable;
    bool     Executable;
} ElfImageSegment;

/*
 * The `bytes` bytes at `file` read as an ELF executable whose segments must lie
 * in [zone_start, zone_end) — both page-aligned, the zone well below the top of
 * the address space. ELF_IMAGE_OK and `out` describes the image, or the first
 * reason it is refused; then only out->Header means anything: the index of the
 * program header the reason is about, or ELF_IMAGE_NO_HEADER. `file` must stay
 * as it is for as long as `out` is used.
 */
ElfImageVerdict ElfImageRead(const void *file, uint64_t bytes, uint64_t zone_start,
                             uint64_t zone_end, ElfImage *out);

/* The next segment to load of an image ElfImageRead accepted, from program
 * header *at on; *at is moved past it. False when there are no more. */
bool ElfImageNextSegment(const ElfImage *image, uint16_t *at, ElfImageSegment *out);

/* The reason a verdict refuses, said so it follows "is not started: ". */
const char *ElfImageVerdictText(ElfImageVerdict verdict);

#endif /* ELF_IMAGE_H */
