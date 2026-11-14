#ifndef IMAGE_LOAD_H
#define IMAGE_LOAD_H

#include "ktypes.h"
#include "error.h"
#include "vmm.h"
#include "image_thread.h"

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
 * Of a box and of an ELF the kernel makes the main strand's thread-local
 * storage from the template the image carries, on the pages after the image
 * (image_thread.h) — no program has to make its own before it can use it.
 *
 * Bytes that are neither are no program, and nothing of them is run as code.
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
 * to every place of the image, where the table of a box's parts in memory is
 * (box_loaded.h; 0 for an image that is no box), the template of its strands'
 * thread-local storage — kept for the cabin (ImageThreadKeep), the cabin's to
 * let go — the main strand's thread pointer, its block already placed after
 * the image (0 when the image keeps nothing per strand), the reserve its
 * main stack is prepared: what a box's hints ask, held between
 * CONFIG_USER_STACK_RESERVE_MIN and _MAX, CONFIG_USER_STACK_RESERVE otherwise —
 * whether its life part says `rise` (rise.h): false for an image that is
 * no box, or says nothing of how it lives — and the authority its asks part
 * asks for, as the AuthTagBit of each word (auth_tags.h): 0 for an image that
 * is no box, or asks for nothing. */
typedef struct {
    uintptr_t   Entry;
    uintptr_t   NameplateVa;
    uint64_t    NameplateBytes;
    uintptr_t   ImageBase;
    uintptr_t   PartsVa;
    ImageThread Thread;
    uintptr_t   ThreadPointer;
    uint64_t    StackReserve;
    bool        Rise;
    uint32_t    Asks;
} ImageLoaded;

/* Whether the image begins as a box or as an ELF — the two kinds this kernel
 * starts, told by their first bytes alone. Nothing about the file's tags
 * says what it is. `name` is what a refusal to read it is said by. */
bool ImageIsProgram(const ImageSource *source, const char *name);

/* `count` bytes of the image from `offset` into `into`, whole or a refusal
 * said with the image's `name`. */
error_t ImageSourceRead(const ImageSource *source, uint64_t offset, void *into, uint64_t count,
                        const char *name);

/*
 * The image `source` loaded into `vmm`: `name` is what every line about it
 * calls it. A load that fails can leave pages mapped in `vmm`; they are the
 * cabin's, freed when it is torn down, and a cabin a load failed in is never
 * started.
 */
error_t ImageLoad(vmm_context_t *vmm, const ImageSource *source, const char *name, ImageLoaded *loaded);

#endif /* IMAGE_LOAD_H */
