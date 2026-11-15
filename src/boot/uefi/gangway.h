#ifndef GANGWAY_H
#define GANGWAY_H

/*
 * What crosses between the files of Gangway, and nothing else.
 *
 * The loader is one EFI image linked from more than one file, each answering
 * for one part of the journey gangway.c walks in order: the firmware's tables,
 * the console, the medium, the screen, memory, the page tables and the
 * handoff; gangway_draw.c draws on the screen what the loader shows itself;
 * gangway_deed.c takes up what a volume's Deed says; gangway_attest.c
 * is the desk the Lectern (gangway_lectern*.c, gangway_succession.c) chooses
 * the volume's kernel at. Every other function and variable stays static to
 * its own file, so a name here is a name another file of the loader really
 * uses — the Lectern's own are gangway_lectern_book.h's.
 */

#include "uefi.h"
/* The volume's own format, shared with the kernel and the tool that writes it
 * — the text they read it by, not a copy of it. <stdint.h> is not available
 * here; uefi.h defines the fixed-width types these headers take from their
 * includer. The book's (its Spine, ribbon and digests) come in with the
 * Lectern's header. */
#include "volume_deed.h"
#include "gangway_lectern.h"

/* Where the kernel is placed and jumped into. */
#define KERNEL_LOAD_ADDR 0x100000ULL

#define PAGE_4KB         0x1000ULL

/* Where the E820 map's entries begin: gangway_memory.c writes them there and
 * gangway_handoff.c names the address in boot_info. */
#define E820_MAP_ADDR    0x504ULL

/* gangway.c: the firmware, as it called this loader. */
extern EFI_SYSTEM_TABLE  *g_st;
extern EFI_BOOT_SERVICES *g_bs;
extern EFI_HANDLE         g_image_handle;

/* gangway_bytes.c: bytes moved, compared and summed with no library under
 * them. Crc32 is ISO 3309's, the sum GPT and a Deed are both specified in. */
void     MemZero(void *dst, size_t n);
void     MemCopy(void *dst, const void *src, size_t n);
int      MemEqual(const void *a, const void *b, size_t n);
int      GuidEqual(const EFI_GUID *a, const EFI_GUID *b);
uint32_t Crc32(const uint8_t *data, uint32_t len);
uint32_t Le32(const uint8_t *p);
uint64_t Le64(const uint8_t *p);

/* gangway_console.c: COM1 by the loader's own port writes — the voice before
 * anything else is reached and after ExitBootServices — and the journal every
 * Print goes into instead of the screen. Panic shows the journal and why on
 * the firmware's console and stops the machine. */
void Com1Init(void);
void Com1Str(const char *s);
void Print(const char *msg);
void PrintDec(uint64_t v);
void PrintHex64(uint64_t v);
void Panic(const char *msg) __attribute__((noreturn));
/* The journal — what was said, kept: its text, how much of it, and how much
 * more was said than it had room for. */
const char *JournalText(uint32_t *bytes, uint32_t *lost);

/*
 * gangway_firmware.c: the firmware's tables, captured at entry
 * (FirmwareTablesTake) and said once the console is clear
 * (FirmwareTablesSay); the ACPI RSDP out of the Configuration Table; the ESRT
 * copied into ACPI NVS before ExitBootServices (EsrtKeep). What boot_info
 * forwards to the kernel is read by gangway_handoff.c.
 */
extern uint64_t g_efi_rt_services_phys;
extern uint32_t g_efi_fw_revision;
extern uint64_t g_efi_system_table_phys;
extern uint64_t g_efi_cfg_table_phys;
extern uint32_t g_efi_cfg_table_count;
extern uint64_t g_efi_esrt_copy_phys;
extern uint32_t g_efi_esrt_copy_size;
void     FirmwareTablesTake(EFI_SYSTEM_TABLE *st);
void     FirmwareTablesSay(EFI_SYSTEM_TABLE *st);
uint64_t FindAcpiRsdp(void);
void     EsrtKeep(void);

/*
 * gangway_medium.c: the medium whose volume is a book this loader boots,
 * found among every Block IO the firmware offers — the device it came from
 * first — and said; EFI_NOT_FOUND, said, when none is. ReadSectors reads the
 * volume on it, 512-byte sectors counted from its ground.
 */
EFI_STATUS BlockIoFindDevice(EFI_HANDLE image_handle);
EFI_STATUS ReadSectors(uint64_t vlba, uint32_t count_512, void *buffer);

/*
 * gangway_deed.c: what the Deed said, filled by DeedTakeUp — the FRAME its
 * book is laid out by; BOOT where the Deed carries one naming a kernel, the
 * kernel tried first, before those its book names by role.
 */
extern VolumeFrame  g_frame;
extern VolumeBoot   g_boot;
extern int          g_have_boot;
extern uint8_t      g_volume_uuid[16];
/* The block the far copy of the volume's Deed sits in: its data run, and
 * every block its book may name, end there. */
extern uint64_t     g_volume_end;
/* The volume's atom (its GEOMETRY, BookAtomOf) in blocks: a Quire of its
 * ribbon begins on one. */
extern uint64_t     g_volume_atom_blocks;

/*
 * Take up the Deed the probe read and checked (gangway_medium.c, FindGroundOn)
 * out of `deed_block`: EFI_SUCCESS when the volume on the ground at
 * `ground_start` is a book this loader boots, said in a line of its own; else
 * why it is not — a Deed with no FRAME, or one whose FRAME, block or BOOT does
 * not hold — is said with the line left open, and the caller ends it naming
 * the medium it passes over (gangway_medium.c, BlockIoProbeVolume).
 */
EFI_STATUS DeedTakeUp(const uint8_t *deed_block, uint64_t ground_start);

/*
 * gangway_attest.c: the volume's kernel, chosen by the Succession
 * — BOOT's first, then the kernels its book names by role — read through
 * `read` (gangway_medium.c's reader of the volume, 512-byte sectors counted
 * from its ground) into pages `bs` gives, and placed at KERNEL_LOAD_ADDR: NULL
 * with its size in *bytes and the end of its pages in *alloc_end — attested,
 * or with nothing to hold it to, said — or the sentence to stop with when no
 * kernel on the volume may be jumped into, every one tried said with why.
 */
typedef EFI_STATUS (*AttestReader)(uint64_t vlba, uint32_t count_512, void *buffer);
const char *SuccessionTake(EFI_BOOT_SERVICES *bs, AttestReader read, uint64_t *bytes, uint64_t *alloc_end);

/* gangway_video.c: the framebuffer the kernel is handed, in the mode chosen
 * for it, said (QueryGopFramebuffer); the framebuffer of the mode the firmware
 * has on now, the mode left as it is and nothing said (GopFramebufferNow).
 * Each all zero when there is none it can be — handed over, the kernel comes
 * up on text. */
typedef struct {
    uint64_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;  /* 0=RGB, 1=BGR — the only two layouts we publish */
} FbInfo;

FbInfo QueryGopFramebuffer(void);
FbInfo GopFramebufferNow(void);

/*
 * gangway_draw.c: text the loader draws on the screen itself, in the cells of
 * the imprint (boxos_imprint_press.h) — the same characters the kernel and the
 * display draw, never the firmware's font. DrawUse names the framebuffer the
 * journey chose (QueryGopFramebuffer); before it, the firmware's mode as it is
 * is drawn on (GopFramebufferNow). DrawEnd clears the screen and draws the end
 * of the pieces — one text, each piece in its colours — as much of it as the
 * screen holds, cut from the front: 0 when there is no framebuffer to draw on.
 */
typedef struct {
    const char *text;   /* UTF-8; '\n' ends a line */
    uint32_t    bytes;
    uint8_t     attr;   /* VIDEO_ATTR_* (video_colors.h) */
} DrawPiece;

void DrawUse(const FbInfo *fb);
int  DrawEnd(const DrawPiece *pieces, uint32_t count);

/*
 * gangway_memory.c: the three handoff pages asked for before anything is
 * written into them; the memory map as E820 at 0x500 before ExitBootServices
 * (its entry count in *e820_count); ExitBootServices with its retries; and,
 * after it, boot_info patched with the map whose key it accepted and the E820
 * rebuilt from that map.
 */
void       ClaimHandoffPages(void);
EFI_STATUS BuildMemoryMap(uint32_t *e820_count);
EFI_STATUS DoExitBootServices(EFI_HANDLE image_handle);
void       PostEbsPatchBootInfo(void);

/* gangway_paging.c: the page tables, one guard page past the kernel's end,
 * and the boot stack above them; and the check that this loader lies inside
 * their identity map. */
extern uint64_t g_pt_base;
extern uint64_t g_stack_base;
void       CheckEfiLoadAddress(void);
EFI_STATUS SetupPageTables(uint64_t kernel_phys_end);

/* gangway_handoff.c: boot_info at BOOT_INFO_ADDR; a step of the journey
 * marked as it is reached (Moment, a word of up to eight letters); the
 * Boarding Pass at BOARDING_PASS_ADDR, written after ExitBootServices so the
 * journal it names is whole; and the jump. */
void Moment(const char *what);
void FillBootInfo(const FbInfo *fb, uint32_t e820_count, uint64_t kernel_end_phys);
void WriteBoardingPass(void);
void JumpToKernel(uint64_t pt_base) __attribute__((noreturn));

#endif /* GANGWAY_H */
