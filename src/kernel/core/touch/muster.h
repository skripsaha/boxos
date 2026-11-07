#ifndef TOUCH_MUSTER_H
#define TOUCH_MUSTER_H

/*
 * The muster — every occurrence this kernel can speak, named before the voyage
 * rather than the first time it is spoken.
 *
 * ‼ WHY THIS EXISTS, measured
 *
 * The Logbook split gave the kernel its own book for the names of things that
 * HAPPEN, separate from the volume's registry of names for things that are
 * STORED. A process asking by name lands in the kernel's book if the name is
 * there and falls through to the volume's registry if it is not — and the
 * fall-through is what broke, because the book was written lazily, the first
 * time the kernel spoke each name.
 *
 * So a process that asked EARLIER than the kernel spoke got a private id out
 * of the volume, and then waited on it. Measured, on an ordinary boot:
 *
 *     intern  'process:died' by pid 2  ->  full=0x50    bare=0x4f
 *     publish 'process:died'           ->  full=0x802a  bare=0x8028
 *
 * The shell interns process:died when it runs its FIRST external command,
 * which on a quiet machine is before anything has died. It then parked on a
 * tag nothing publishes to, for ever — so the prompt never came back and the
 * machine looked dead from the second command onward. And on the way there it
 * wrote 'process' and 'process:died' into the VOLUME's tag registry, which is
 * the list of names for files on somebody's medium.
 *
 * A vocabulary is something you have, not something that appears the first
 * time you use a word. Mustering the names at the start closes both: the
 * lookup always hits, so the id is the same whoever asks first, and the name
 * never reaches the volume's registry to pollute it.
 *
 * ‼ THIS LIST CANNOT BE COMPLETE, AND DOES NOT PRETEND TO BE
 *
 * Some names are built at runtime from data — "pci:vendor:8086" is one per
 * device the machine has. Those families are mustered by their bare key only.
 * Anything the kernel speaks that is not here is said out loud once, by name,
 * so a forgotten entry is a line in the log rather than a shell that hangs.
 *
 * ‼ ONE KEY HERE IS THE VOLUME'S OWN: system
 *
 * system:shutdown and system:reboot are the halt's word (system_halt.c), and a
 * name is mustered with its bare key, so "system" — the first of TagFS's
 * reserved keys, the auth key of id 0 (tagfs_reserved.h) — is the kernel's
 * book's from boot: a process asking by "system", or by any "system:X", lands
 * here and hears the halt's word, never a file's write or anchor TagFS
 * publishes on id 0. Unmustered, a process that asked for the halt's word
 * before the halt held the volume's ids and heard nothing (logcheck halthog),
 * and the halt's first publish took "system" into this book all the same.
 * Nothing in the tree listens to files by "system".
 *
 * Keep it sorted. It is read by people.
 */
#define TOUCH_KERNEL_MUSTER(X)          \
    X("acpi:boot-error")                \
    X("acpi:cper:generic")              \
    X("acpi:cper:memory")               \
    X("acpi:cper:pcie")                 \
    X("acpi:cper:processor")            \
    X("acpi:power-button")              \
    X("acpi:ready")                     \
    X("acpi:sleep:s1")                  \
    X("acpi:sleep:s2")                  \
    X("acpi:sleep:s3")                  \
    X("acpi:sleep:s4")                  \
    X("aml:ready")                      \
    X("apei:generic:error")             \
    X("apei:ghes:ready")                \
    X("apei:memory:error")              \
    X("apei:pcie:error")                \
    X("apei:processor:error")           \
    X("cet:enabled")                    \
    X("cet:fault:cp")                   \
    X("efi:capsule:armed")              \
    X("efi:capsule:delivered")          \
    X("efi:capsule:set-fail")           \
    X("efi:variable:nv-info")           \
    X("esrt:ready")                     \
    X("ibt:enabled")                    \
    X("iommu:dma:mapped")               \
    X("iommu:dma:unmapped")             \
    X("iommu:domain:attached")          \
    X("iommu:fault")                    \
    X("iommu:ready")                    \
    X("keyboard")                       \
    X("mce:fault:detected")             \
    X("mce:fault:fatal")                \
    X("mce:fault:recovered")            \
    X("mce:migration:completed")        \
    X("mce:migration:failed")           \
    X("mce:migration:unmapped")         \
    X("memtag:cabin:granted")           \
    X("memtag:cabin:revoked")           \
    X("memtag:fault:denied")            \
    X("memtag:guard:set")               \
    X("memtag:region:added")            \
    X("memtag:region:released")         \
    X("memtag:tag:applied")             \
    X("memtag:tag:cleared")             \
    X("numa:topology")                  \
    X("pku:fault:denied")               \
    X("process:died")                   \
    X("process:spawned")                \
    X("screen")                         \
    X("seat:emptied")                   \
    X("seat:ready")                     \
    X("seat:taken")                     \
    X("secureboot:audit-mode")          \
    X("secureboot:deployed-mode")       \
    X("secureboot:off")                 \
    X("secureboot:on")                  \
    X("secureboot:setup-mode")          \
    X("secureboot:user-mode")           \
    X("shstk:enabled")                  \
    X("storage:ata:error")              \
    X("strand:exited")                  \
    X("strand:parked")                  \
    X("strand:spawned")                 \
    X("strand:woken")                   \
    X("system:reboot")                  \
    X("system:shutdown")                \
    X("tme:keyid:rekey:failed")         \
    X("tme:pool:ready")                 \
    X("usb:arrived")                    \
    X("usb:connect")                    \
    X("usb:disconnect")                 \
    X("usb:left")                       \
    X("volume:mounted")                 \
    X("watchtest:inner")                \
    X("watchtest:one")                  \
    /* Families built at runtime — the bare key only, because the rest of the \
     * name is one per device or per firmware object on the machine. */       \
    X("pci")                            \
    X("esrt")

#endif /* TOUCH_MUSTER_H */
