#ifndef TOUCH_LOGBOOK_H
#define TOUCH_LOGBOOK_H

#include "touch.h"

/*
 * TouchLogbook — the kernel's own event vocabulary.
 *
 * A volume's tag registry is a cargo manifest: it names what is STORED.
 * The Logbook is the ship's log: it names what HAPPENED. Two books, and
 * they must never share page numbers.
 *
 * Until this existed they did share them. Every kernel occurrence —
 * `mce:fault:fatal`, `apei:ghes:ready`, `usb:arrived`, `keyboard` — was
 * interned into the mounted volume's registry, which produced three
 * separate defects, all measured:
 *
 *   1. Occurrences resolved BEFORE the volume mounted (tagfs_init lives in
 *      storage_deck_init, main.c) got TOUCH_TAG_INVALID and stayed dead for
 *      the life of the boot. That is most of the kernel's vocabulary on
 *      EVERY boot, healthy volume or not — mce, apei, cet, tme, pku,
 *      mce:migration, acpi:ready, aml:ready, display:ready and the early
 *      memtag traffic. Every publish through them was a silent no-op.
 *
 *   2. A machine with no volume lost the vocabulary entirely, which is how
 *      a keyboard that had enumerated correctly on a live board still could
 *      not deliver a keystroke: no volume -> no registry -> no `keyboard`
 *      tag -> TouchPublishIrqPair published to TOUCH_TAG_INVALID.
 *
 *   3. Occurrences CONSUMED on-disk tag ids and were flushed to the medium,
 *      including data-driven ones — `pci:vendor:8086:1234` per device,
 *      `secureboot:db-cert:<sha256>` per certificate. A machine's hardware
 *      inventory ended up in the file system's tag vocabulary, and the ids
 *      it took were the ids the volume's own tags wanted. That is what made
 *      a file tagged `display` come back tagged `mce`.
 *
 * The split is structural, not conventional. Bit 15 of a TouchTag says which
 * book the id came from:
 *
 *      0x0000..0x7FFE   the volume's registry — properties of stored things
 *      0x8000..0xFFFE   this Logbook          — occurrences the kernel saw
 *      0xFFFF           TOUCH_TAG_INVALID
 *
 * TAGFS_MAX_TAG_ID caps the volume side at 0x7FFE, so the volume cannot
 * reach into the kernel half even in principle. Touch's bucket table is
 * already two-level over the whole 16-bit space, so the kernel half costs
 * one lazily-allocated leaf and nothing else.
 *
 * WHO WRITES: the kernel, and only the kernel. TouchLogbookResolve /
 * TouchLogbookIntern create entries; they are reached from kernel publishers
 * (TouchPublish, TouchLogbookIntern) and from drivers that cache handles at init.
 * A process never writes here — SysTouchIntern reaches the Logbook through
 * TouchTagResolve, which LOOKS UP the kernel half and falls through to the
 * volume for everything else. The kernel owns what the kernel named; a
 * process can hear it, and cannot forge it.
 *
 * ORDERING: the Logbook needs no init call and no boot phase. It builds
 * itself on first use, which is the earliest kernel resolve (mce_init, long
 * before any file system). That is the whole point — the vocabulary must not
 * depend on a medium, because the machine that failed to mount one is
 * exactly the machine you need to hear from.
 *
 * NOT IRQ-SAFE, by the same rule as the volume registry: interning takes a
 * lock and can kmalloc. Drivers resolve their handles at init and pass the
 * cached ids to TouchPublishIrqPair, which is IRQ-safe.
 */

/* Bit 15 marks an id issued by this Logbook. */
#define TOUCH_TAG_KERNEL_BIT   ((TouchTag)0x8000u)

/* Highest index this Logbook can issue: id 0xFFFF is TOUCH_TAG_INVALID. */
#define TOUCH_LOGBOOK_MAX_INDEX 0x7FFEu

static inline bool TouchTagIsKernel(TouchTag tag_id)
{
    return tag_id != TOUCH_TAG_INVALID && (tag_id & TOUCH_TAG_KERNEL_BIT) != 0;
}

/* Kernel door — name an occurrence, creating the entry if it is new.
 * Mirrors TouchTagResolve's contract: *out_bare is the (key, NULL) id and
 * also serves wildcard "key:..." listeners; *out_full is the (key, value)
 * id, left TOUCH_TAG_INVALID for a bare tag or an explicit "key:..." wildcard.
 * Either may come back invalid if the Logbook is out of memory. */
void TouchLogbookResolve(const char *tag, TouchTag *out_full, TouchTag *out_bare);

/* Single-id convenience: the full id when the tag carries a value, else the
 * bare one. What kernel publishers that do not care about wildcards want. */
TouchTag TouchLogbookIntern(const char *tag);

/* Reader door — find only, never create. TouchTagResolve consults this
 * before the volume registry so a process subscribing by name lands on the
 * same id the kernel publishes to. */
void TouchLogbookLookup(const char *tag, TouchTag *out_full, TouchTag *out_bare);

/* Reverse lookup for diagnostics: the key of `tag_id`, or NULL if the id is
 * not one of ours. `out_value` receives the value or NULL for a bare tag. */
const char *TouchLogbookName(TouchTag tag_id, const char **out_value);

/* Number of names in the Logbook. */
uint32_t TouchLogbookCount(void);

#endif /* TOUCH_LOGBOOK_H */
