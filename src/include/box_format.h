#ifndef BOX_FORMAT_H
#define BOX_FORMAT_H

/*
 * box — the BoxOS program file: a box of named parts.
 *
 * A program is not "code with things attached". It is a box, and code is one
 * of the kinds of part a box can hold: beside it what the program asks of the
 * processor, its Form, its settings, its names for its own functions, what it
 * tells people about itself. Anyone can put a new kind of part in a box; the
 * system knows a few. A part this system does not know is passed over — unless
 * the box marks it MUST_UNDERSTAND, and then the box is refused with the
 * honest reason: "this program needs X, which this system does not know".
 *
 * ‼ ONE WAY TO WRITE A BOX. Header, table, names, parts, in that order and
 * nothing between them but zero bytes of alignment:
 *
 *   header   BOX_HEADER_BYTES at offset 0 (header_size can grow; the table
 *            starts where it says the header ends)
 *   table    part_count entries of BOX_ENTRY_BYTES, strictly ascending by
 *            name — the order every list of tags in BoxOS keeps (BoxNameOrder)
 *   names    every part's name, in table order, back to back, no NULs
 *   parts    in table order, each at the first offset its alignment allows
 *            after the one before: BOX_LOADABLE_ALIGN for code, const and
 *            data — they are read straight into pages — BOX_PART_ALIGN for
 *            every other; the last part ends the file
 *
 * Every byte of the file is in exactly one of these or is a zero byte of
 * alignment. The same parts make the same bytes.
 *
 * ‼ THE SEAL is SHA-256 over the file from its first byte to the end of the
 * names, with the 32 bytes of the seal itself read as zeros. It covers the
 * table, and the table holds each part's own SHA-256 — so the seal stands for
 * every byte of the box, and a reader checks only the parts it reads.
 *
 * All numbers are little-endian. Integer types come from the includer:
 *   Kernel:     #include "ktypes.h"
 *   Userspace:  #include "box/defs.h"
 *   Host tool:  #include <stdint.h> and <stdbool.h>
 * Read with box_reader.h; written with box_pen.h. Nothing else reads or writes
 * the layout below.
 */

/* ── Header ──────────────────────────────────────────────────────────────── */

/* 89 'B' 'O' 'X' 0D 0A 1A 0A — "BOX" to the eye, and, as PNG's, a high byte
 * a 7-bit channel strips and a CR LF a text transfer rewrites. */
#define BOX_MAGIC_0  0x89u
#define BOX_MAGIC_1  0x42u
#define BOX_MAGIC_2  0x4Fu
#define BOX_MAGIC_3  0x58u
#define BOX_MAGIC_4  0x0Du
#define BOX_MAGIC_5  0x0Au
#define BOX_MAGIC_6  0x1Au
#define BOX_MAGIC_7  0x0Au

#define BOX_HEADER_BYTES   80u
#define BOX_AT_MAGIC       0u    /* u8[8] */
#define BOX_AT_HEADER_SIZE 8u    /* u32: the header's length; this tree writes 80 */
#define BOX_AT_PART_COUNT  12u   /* u32 */
#define BOX_AT_TABLE       16u   /* u64: where the table starts */
#define BOX_AT_NAMES       24u   /* u64: where the names start */
#define BOX_AT_NAMES_SIZE  32u   /* u64 */
#define BOX_AT_FILE_SIZE   40u   /* u64: the file's whole length */
#define BOX_AT_SEAL        48u   /* u8[32] */
#define BOX_SEAL_BYTES     32u

/* ── The table ───────────────────────────────────────────────────────────── */

#define BOX_ENTRY_BYTES    80u
#define BOX_ENTRY_OFFSET   0u    /* u64: where the part's bytes are in the file */
#define BOX_ENTRY_SIZE     8u    /* u64: how many bytes it has in the file */
#define BOX_ENTRY_MEMORY   16u   /* u64: how many it takes in memory */
#define BOX_ENTRY_PLACE    24u   /* u64: a loadable part's offset from the image base */
#define BOX_ENTRY_FLAGS    32u   /* u32: BOX_FLAG_* */
#define BOX_ENTRY_NAME_AT  36u   /* u32: offset in the names */
#define BOX_ENTRY_NAME_LEN 40u   /* u16 */
#define BOX_ENTRY_RESERVED 42u   /* u8[6]: zero, up to the hash */
#define BOX_ENTRY_HASH     48u   /* u8[32]: SHA-256 of the part's bytes in the file */
#define BOX_HASH_BYTES     32u

/* The part must be understood, or the box is not started. */
#define BOX_FLAG_MUST_UNDERSTAND  0x1u
/* A part that describes the program, put into its memory read-only when it
 * starts and named in its table of loaded parts (form, settings). */
#define BOX_FLAG_LOAD_AT_START    0x2u
/* The bytes are compressed. Never on a part the kernel maps or reads to start
 * the program — they are read into pages as they are, or not at all. */
#define BOX_FLAG_COMPRESSED       0x4u
#define BOX_FLAGS_KNOWN           0x7u

#define BOX_PART_ALIGN      8u
#define BOX_LOADABLE_ALIGN  4096u

/* The highest a loadable part may reach above the image base: the user half
 * of the address space. Past it no base could hold the image. */
#define BOX_PLACE_LIMIT     0x0000800000000000ull

/* A part's name is a tag: a key, and after a ':' a value that says more
 * ("icon:64", "about:ru"). Each side at most this many bytes. */
#define BOX_NAME_SIDE_MAX   255u

/* ── The standard's parts ────────────────────────────────────────────────── */

typedef enum {
    BOX_KIND_UNKNOWN = 0,   /* the program's own, or a standard this system does not know */
    BOX_KIND_CODE,          /* machine code: R+X in memory; required */
    BOX_KIND_CONST,         /* read-only data: R */
    BOX_KIND_DATA,          /* data: R+W; memory past the file's bytes is zeros */
    BOX_KIND_FIXUPS,        /* where to add the base: the box can go anywhere */
    BOX_KIND_DOORS,         /* named ways in; `start` required */
    BOX_KIND_THREAD,        /* the template of each thread's local storage */
    BOX_KIND_NAMES,         /* the program's names for its functions (a Nameplate) */
    BOX_KIND_SIGNATURE,     /* an author's signature over the seal — not yet read */
    BOX_KIND_NEEDS,         /* text: what the program needs of the processor */
    BOX_KIND_HINTS,         /* text: what it usually needs; never a limit raised */
    BOX_KIND_LIFE,          /* text: how it lives — task, stays, serves */
    BOX_KIND_FORM,          /* text: what it takes and gives */
    BOX_KIND_SETTINGS,      /* text: the settings it declares */
    BOX_KIND_RECIPE,        /* text: how it was made */
    BOX_KIND_ABOUT,         /* text: what it tells people about itself */
    BOX_KIND_ICON,          /* icon:N — an N by N picture */
    BOX_KIND_WORKS,         /* text: which files it works with, by their tags */
    BOX_KIND_ASKS,          /* text: what it asks to be allowed, and why */
    BOX_KIND_ROLE,          /* text: what the volume's maker keeps the file as — system or boot */
    BOX_KIND_COUNT
} BoxKind;

/* ── doors ───────────────────────────────────────────────────────────────── *
 *
 *   u32 count, u32 zero
 *   count entries of 16 bytes, strictly ascending by name:
 *       u64 offset      from the image base, inside `code`'s bytes
 *       u32 name_at     offset in the door names below
 *       u16 name_length
 *       u16 zero
 *   the door names, in entry order, back to back
 */
#define BOX_DOORS_HEAD_BYTES   8u
#define BOX_DOOR_BYTES         16u
#define BOX_DOOR_START         "start"

/* ── thread ──────────────────────────────────────────────────────────────── *
 *
 *   u64 align        a power of two, at most BOX_THREAD_ALIGN_MAX
 *   u64 init_bytes   bytes of the template that are not zero
 *   u64 total_bytes  the whole block, init_bytes and the zeros after them
 *   u64 zero
 *   init_bytes bytes
 */
#define BOX_THREAD_HEAD_BYTES  32u
#define BOX_THREAD_ALIGN_MAX   4096u

/* ── fixups ──────────────────────────────────────────────────────────────── *
 *
 * u64 words, the encoding of RELR (the ELF gABI's relative relocations): a
 * word with bit 0 clear is the offset of a fixup, and the place after it is
 * where the next word goes on from; a word with bit 0 set is a bitmap over the
 * 63 places from there on, bit k meaning the place k-1 words further, after
 * which it goes on 63 words further. A fixup adds the image base to the u64 at
 * its offset. The offsets rise, every one lies in the bytes a loadable part
 * has in the file, and none is the half of a word.
 */
#define BOX_FIXUP_BITS  63u

#endif /* BOX_FORMAT_H */
