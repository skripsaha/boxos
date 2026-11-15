#ifndef BOX_LOADED_H
#define BOX_LOADED_H

/*
 * What is where — the table of a box's parts in its program's memory.
 *
 * The kernel writes it once, when it starts a box, on read-only pages of the
 * program's own image, and CabinInfo says where (parts_addr). It names every
 * part the program finds already in its memory: its code, constants and data,
 * its names, and each part the box marks to be loaded at start (form,
 * settings, …), of a kind the kernel knows or not — those it puts into memory
 * as they are, never to be written or run. Every other part of the box stays
 * in the file, and the program reads it from there (CabinInfo.program_file);
 * the seal says whether the file is still the box the program was started
 * from.
 *
 *   BoxLoadedHead                                    40 bytes
 *   BoxLoadedPart[Count]       in the order of the box's table: by name
 *   names                      each name's bytes at NameAt, no terminator
 *
 * Everything lies inside the table's Bytes. A program started from an ELF has
 * no table: parts_addr is 0.
 *
 * Integer types come from the includer, as everywhere in src/include.
 */

#define BOX_LOADED_SEAL_BYTES 32u

typedef struct {
    uint32_t Count;                         /* parts named */
    uint32_t Bytes;                         /* the whole table: head, parts and names */
    uint8_t  Seal[BOX_LOADED_SEAL_BYTES];   /* the seal of the box it was started from */
} BoxLoadedHead;

typedef struct {
    uint64_t Address;      /* where the part is in memory */
    uint64_t Size;         /* its bytes there — for data, its zeros too */
    uint32_t NameAt;       /* its name, from the table's start */
    uint32_t NameLength;
} BoxLoadedPart;

_Static_assert(sizeof(BoxLoadedHead) == 40, "the head of the table of loaded parts is 40 bytes");
_Static_assert(sizeof(BoxLoadedPart) == 24, "a loaded part is named in 24 bytes");

#endif /* BOX_LOADED_H */
