#ifndef IMAGE_LOAD_H
#define IMAGE_LOAD_H

#include "ktypes.h"
#include "error.h"
#include "vmm.h"

struct TagFSFileHandle;

/*
 * A program's image put into a cabin — the kinds of image this kernel starts,
 * told apart by what the bytes are and what the volume says about them, never
 * guessed.
 *
 *   box  begins 89 'B' 'O' 'X' (box_format.h): the BoxOS program, read the one
 *        way it is written (box_reader.h) and only as far as starting it
 *        needs — its header, its table, the parts it is started by. Each part
 *        of its image goes from the file straight into the pages it will live
 *        in, a page at a time, its hash taken as it comes; the box is placed
 *        at a base of the kernel's choosing when it carries fixups, where it
 *        says otherwise; every page gets exactly its rights. The parts it
 *        marks to be loaded at start follow the image read-only, and a table
 *        of what is where names them all to the program. box_load.c.
 *
 *   ELF  begins 7F 'E' 'L' 'F', and is read whole before anything is mapped
 *        (elf_image.h). Each segment gets pages of its own with exactly its
 *        rights: R, R+W or R+X — a segment that asks for W+X is refused.
 *
 *   raw  code run from its first byte, the image byte for byte at the code
 *        start. Only when the volume declares it so with the tag `raw`: bytes
 *        that are no program this kernel knows are not run as code on the
 *        strength of being here. Raw code has no segments to keep apart, so its
 *        pages are writable and executable at once, and the journal says so
 *        each time one starts.
 *
 * A box or an ELF tagged raw is refused: the bytes say one thing and the
 * volume another, and starting either reading would be a guess.
 *
 * Every refusal is said in the journal with the image's name and the reason,
 * and answered ERR_INVALID_ELF — the image is no program that can be started;
 * ERR_NO_MEMORY, ERR_SPAWN_FAILED or the volume's own answer when one could,
 * and the machine could not.
 */

/* Where an image's bytes come from: a file on the volume, read at whatever
 * place the loader asks — a box is never read whole — or bytes the caller
 * already holds. */
typedef struct {
    uint64_t                Bytes;    /* the image's length */
    struct TagFSFileHandle *File;     /* opened to read, or NULL ... */
    const uint8_t          *Memory;   /* ... and these bytes instead */
    uint64_t                FileId;   /* the volume's id of File; 0 with Memory */
} ImageSource;

/* What a load leaves for the process: where it starts, where the image's
 * names for its functions are mapped (0 when it carries none), what was added
 * to every place of the image, and where the table of a box's parts in memory
 * is (box_loaded.h; 0 for an image that is no box). */
typedef struct {
    uintptr_t Entry;
    uintptr_t NameplateVa;
    uint64_t  NameplateBytes;
    uintptr_t ImageBase;
    uintptr_t PartsVa;
} ImageLoaded;

/* `count` bytes of the image from `offset` into `into`, whole or a refusal
 * said with the image's `name`. */
error_t ImageSourceRead(const ImageSource *source, uint64_t offset, void *into, uint64_t count,
                        const char *name);

/*
 * The image `source` loaded into `vmm`: `name` is what every line about it
 * calls it, `tagged_raw` whether the volume declares it raw code. A load that
 * fails can leave pages mapped in `vmm`; they are the cabin's, freed when it
 * is torn down, and a cabin a load failed in is never started.
 */
error_t ImageLoad(vmm_context_t *vmm, const ImageSource *source, const char *name, bool tagged_raw,
                  ImageLoaded *loaded);

#endif /* IMAGE_LOAD_H */
