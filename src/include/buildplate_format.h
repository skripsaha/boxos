#ifndef BOXOS_BUILDPLATE_FORMAT_H
#define BOXOS_BUILDPLATE_FORMAT_H

/*
 * BuildPlate — what this kernel was made from, riveted to the kernel.
 *
 * A machine part carries a nameplate with its name on it (see
 * nameplate_format.h). The same part carries a SECOND plate, stamped by
 * whoever built it: which drawings it was built to, on what day, with which
 * options fitted. This is that plate, and the reason it exists is a run on
 * live hardware that had to be thrown away.
 *
 * On 2026-09-22 an image was written to a stick and booted on a laptop. 1228
 * of the 2274 lines it printed came from an injection the test harness had
 * made into TouchLogbookResolve — and by the time the log was read, the
 * harness had put the source back. `git status` was clean. The image was not
 * the tree. Nothing in the image, and nothing the kernel said, could tell
 * anybody that: every timing conclusion drawn from that run was worthless and
 * there was no way to know it from the log.
 *
 * So the image carries its own pedigree, and the kernel says it in its FIRST
 * LINE, before anything else can scroll it away:
 *
 *   TreeFingerprint  one number over every file the build reads — every
 *                    source, every header, every asm file, the Makefile, the
 *                    host tools. Recomputing it later answers "is the tree in
 *                    front of me the tree this image was made from" with a
 *                    yes or a no, and nothing else answers it at all.
 *   Keys             the build keys AS GIVEN. Eleven of them change what the
 *                    compiler sees, and until now the only record of which
 *                    ones were used was the NAME of a marker file under
 *                    build/, which `make clean` deletes and which never
 *                    leaves the host.
 *   Verdict          clean, or UNDER TEST: this image was packed while the
 *                    harness had the tree mutated. A run from an UNDER TEST
 *                    image is a run whose log is evidence about the harness
 *                    and not about the kernel.
 *   StampedUnix      when the plate was riveted, which is when the kernel was
 *                    linked.
 *
 * HOW IT IS FOUND, and why it is a tag rather than a section. The kernel links
 * with --oformat=binary: there are no sections in kernel.bin, so a tool cannot
 * ask the ELF where the plate is. It finds the plate by its Tag — sixteen
 * bytes that appear exactly once in the image — and refuses to stamp anything
 * where the count is not one. The Tag is a char array and never a string
 * literal, so nothing merges it and nothing else in the image carries it.
 *
 * Three consumers share this one definition:
 *   tools/buildplate.c            computes, stamps and verifies, on the host
 *   kernel core/buildplate        says it, in the first line of the log
 *   tools/create_tagfs.c          carries the same VERDICT into the volume's
 *                                 own Deed stamp, for the half of the
 *                                 shipment the kernel does not contain
 *
 * Integer types come from the includer, as everywhere in src/include:
 *   Kernel:     #include "ktypes.h"
 *   Host tool:  #include <stdint.h>
 */

#include "boxos_magic.h"

#define BUILDPLATE_VERSION      1u

/* Sixteen bytes, no terminator: this is a tag, not a string. */
#define BUILDPLATE_TAG_BYTES    16
#define BUILDPLATE_TAG_INIT     { 'B','O','X','O','S',' ','B','U','I','L','D', \
                                  'P','L','A','T','E' }

/* Room for every key the Makefile can be given, spelled out as `NAME=value`
 * pairs separated by spaces. Fourteen keys at around a dozen characters each,
 * with room to grow; the tool refuses to stamp a longer list rather than
 * truncating one, because a truncated key list is a key list that lies. */
#define BUILDPLATE_KEYS_MAX     208

#define BUILDPLATE_VERDICT_UNSTAMPED   0u  /* nothing has been riveted here */
#define BUILDPLATE_VERDICT_CLEAN       1u  /* the tree carried no injection */
#define BUILDPLATE_VERDICT_UNDER_TEST  2u  /* packed over a harness injection */

typedef struct BuildPlate {
    char     Tag[BUILDPLATE_TAG_BYTES]; /* BUILDPLATE_TAG_INIT — how it is found */
    uint32_t Magic;                     /* BUILDPLATE_MAGIC */
    uint32_t Version;                   /* BUILDPLATE_VERSION */
    uint64_t TreeFingerprint;           /* FNV-1a 64 over every input file */
    uint64_t StampedUnix;               /* when it was riveted */
    uint32_t FileCount;                 /* how many files the fingerprint spans */
    uint32_t Verdict;                   /* BUILDPLATE_VERDICT_* */
    char     Keys[BUILDPLATE_KEYS_MAX]; /* NUL-terminated */
} BuildPlate;

#endif /* BOXOS_BUILDPLATE_FORMAT_H */
