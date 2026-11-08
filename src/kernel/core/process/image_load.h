#ifndef IMAGE_LOAD_H
#define IMAGE_LOAD_H

#include "ktypes.h"
#include "error.h"
#include "vmm.h"

/*
 * A program's image put into a cabin — the kinds of image this kernel starts,
 * told apart by what the bytes are and what the volume says about them, never
 * guessed.
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
 * An ELF tagged raw is refused: the bytes say one thing and the volume another,
 * and starting either reading would be a guess.
 *
 * Every refusal is said in the journal with the image's name and the reason,
 * and answered ERR_INVALID_ELF — the image is no program that can be started;
 * ERR_NO_MEMORY or ERR_SPAWN_FAILED when one could, and the machine could not.
 */

/*
 * The `bytes` bytes at `image` — the caller's, only read, and needed no longer
 * than the call — loaded into `vmm`: `name` is what every line about it calls
 * it, `tagged_raw` whether the volume declares it raw code. *entry is where the
 * program starts. A load that fails can leave pages mapped in `vmm`; they are
 * the cabin's, freed when it is torn down, and a cabin a load failed in is
 * never started.
 */
error_t ImageLoad(vmm_context_t *vmm, const void *image, uint64_t bytes, const char *name,
                  bool tagged_raw, uintptr_t *entry);

#endif /* IMAGE_LOAD_H */
