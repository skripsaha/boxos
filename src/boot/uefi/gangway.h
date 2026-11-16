#ifndef GANGWAY_H
#define GANGWAY_H

/*
 * What crosses between the files of Gangway, and nothing else.
 *
 * The loader is one EFI image linked from more than one file, each answering
 * for one part of the journey gangway.c walks in order: the firmware's tables,
 * the console, the medium, the screen, memory, the page tables and the
 * handoff; gangway_draw.c draws on the screen what the loader shows itself;
 * gangway_keys.c reads the keyboard and gangway_panel*.c is the panel a
 * person asks for by holding Space;
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
#include "boarding_pass.h"
#include "boxos_crossings.h"
#include "boxos_imprint.h"

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
 * Print goes into instead of the screen; while PrintAloud is on, down COM1 as
 * well. Panic shows the journal and why on the screen and stops the machine. */
void Com1Init(void);
void Com1Str(const char *s);
void Print(const char *msg);
void PrintAloud(int on);
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
/* The same medium counted from its first sector, not the volume's: MediumRead;
 * one 4 KiB slot written at a 4 KiB-aligned sector and flushed
 * (MediumWriteSlot) — EFI_WRITE_PROTECTED from a medium that refuses writes,
 * EFI_UNSUPPORTED from one whose blocks are neither 512 nor 4096 bytes; and
 * whether the MBR names every partition at `end` or past it (MediumFrontFree,
 * 0 for a GPT's protective table). */
EFI_STATUS MediumRead(uint64_t lba_512, uint32_t count_512, void *buffer);
EFI_STATUS MediumWriteSlot(uint64_t lba_512, const void *slot);
int        MediumFrontFree(uint64_t end);

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
 * kernel on the volume may be jumped into, every one tried said with why. A
 * kernel chosen, gangway.settings is read in the same reading of the book and
 * handed to SettingsTake.
 */
typedef EFI_STATUS (*AttestReader)(uint64_t vlba, uint32_t count_512, void *buffer);
const char *SuccessionTake(EFI_BOOT_SERVICES *bs, AttestReader read, uint64_t *bytes, uint64_t *alloc_end);
/* The kernel SuccessionTake chose, as the Lectern told it (gangway_lectern.h). */
const LecternKernel *KernelChosen(void);

/*
 * gangway_settings.c: what the person set in gangway.settings
 * (boxos_gangway_settings.h), read by the one reader every program's settings
 * are read by — taken from the file's bytes (SettingsTake), every line that
 * does not fit said; with no file, everything as Gangway came. `set` says
 * which the file sets — a value of its own, not the word default: only those
 * go to the kernel, as stamps, and the kernel goes its own way in the rest.
 * The panel sets them as well, for one start. SettingsList says those of
 * `which` as they stand — "screen 1920x1080, cores 2" — and whether it said
 * any.
 */
#define GANGWAY_SET_SCREEN   1u
#define GANGWAY_SET_STARTUP  2u
#define GANGWAY_SET_CORES    4u
#define GANGWAY_SET_KEEP_LOG 8u
#define GANGWAY_SET_SCENE    16u
#define GANGWAY_SET_TEXT     32u
#define GANGWAY_SET_THEME    64u

/* The panel's colours (theme). */
#define GANGWAY_THEME_DARK  0u
#define GANGWAY_THEME_LIGHT 1u

/* The longest text of tags the person may ask the scene by, and how many. */
#define SCENE_TAGS_BYTES 256u
#define SCENE_ASKED_MOST 32u

typedef struct {
    uint32_t set;             /* GANGWAY_SET_* */
    uint32_t screen_width;    /* the screen asked for — 0 by 0 for the monitor's own (auto) */
    uint32_t screen_height;
    uint8_t  startup;         /* BOARDING_STARTUP_* (boarding_pass.h) */
    uint8_t  keep_log;        /* 1: what the machine says is kept through a reset */
    uint8_t  scene;           /* 1: the scene's first frame drawn as the machine starts (gangway_scene.c) */
    uint32_t cores;           /* the most processors the kernel starts, 0 for every one */
    uint32_t text_size;       /* how tall the panel's letters are, px: 0 for the screen to say (auto) */
    uint8_t  theme;           /* GANGWAY_THEME_* */
    /* The tags the person asks the scene by — none: boot.scene, by its name —
     * their text, and its words: SCENE_ASKED_MOST at the most. */
    char        scene_tags[SCENE_TAGS_BYTES];
    const char *scene_asked[SCENE_ASKED_MOST];
    uint32_t    scene_nasked;
} GangwaySettings;

extern GangwaySettings g_settings;
void SettingsTake(const uint8_t *text, uint64_t bytes);
int  SettingsList(uint32_t which);

/*
 * gangway_crossings.c: the starts kept in the medium's front
 * (boxos_crossings.h). CrossingsTake reads them, once; CrossingsMisses is how
 * many in a row from the newest did not land, CrossingsNone why none are kept
 * here (NULL when they are), CrossingsNext the number this start carries, and
 * CrossingsKept the crossings that check out, newest first. CrossingKeep
 * writes this start's — `panel` CROSSING_PANEL_*, `fb` the screen handed on —
 * and CrossingStamp names it for the kernel: 0 when none was written.
 */
void        CrossingsTake(void);
uint32_t    CrossingsMisses(void);
const char *CrossingsNone(void);
uint64_t    CrossingsNext(void);
uint32_t    CrossingsKept(const Crossing **list);
int         CrossingStamp(BoardingCrossing *out);


/* gangway_video.c: the framebuffer the kernel is handed, in the mode chosen
 * for it — the size the person asks for (g_settings.screen_*) when the adapter
 * has it — said (QueryGopFramebuffer), and how it was chosen, in words
 * (GopChosenWhy); the framebuffer of the mode the firmware has on now, the
 * mode left as it is and nothing said (GopFramebufferNow). Each all zero when
 * there is none it can be — handed over, the kernel comes up on text. GopSizes
 * lists the sizes the adapter's modes come in that the kernel can be handed,
 * each once, smallest first: how many, at most `most`. */
typedef struct {
    uint64_t addr;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;  /* 0=RGB, 1=BGR — the only two layouts we publish */
} FbInfo;

typedef struct {
    uint32_t width;
    uint32_t height;
} GopSize;

FbInfo      QueryGopFramebuffer(void);
void        CrossingKeep(uint8_t panel, const FbInfo *fb);   /* gangway_crossings.c, above */
FbInfo      GopFramebufferNow(void);
const char *GopChosenWhy(void);
uint32_t    GopSizes(GopSize *sizes, uint32_t most);

/*
 * gangway_scene.c: the scene the machine starts with (boxos_scene.h).
 * SceneTake holds boot.scene, as the Lectern read it, to its bounds, and keeps
 * it; SceneShow draws its first frame on the screen the kernel is handed, or
 * says why not, and gives its pages back either way; SceneStamp names the
 * scene for the kernel when its frame is on the screen: 0 when none is.
 */
void SceneTake(const LecternCalled *called);
void SceneShow(const FbInfo *fb);
int  SceneStamp(BoardingScene *out);

/*
 * gangway_draw.c: text the loader draws on the screen itself, in the cells of
 * the imprint (boxos_imprint_press.h) — the same characters the kernel and the
 * display draw, never the firmware's font. DrawUse names the framebuffer the
 * journey chose (QueryGopFramebuffer); before it, the firmware's mode as it is
 * is drawn on (GopFramebufferNow). The pieces are one text, each piece in its
 * colours. DrawPage draws its rows from row `skip` onto `height` rows of the
 * screen from row `top`, every cell of them, and answers how many rows the
 * whole text fills; DrawCount only counts them. DrawEnd draws the end of the
 * pieces on the whole screen, as much as it holds, cut from the front: 0 when
 * there is no framebuffer to draw on. DrawRows and DrawCols: the screen's
 * cells, 0 when there is none.
 */
typedef struct {
    const char *text;   /* UTF-8; '\n' ends a line */
    uint32_t    bytes;
    uint8_t     attr;   /* VIDEO_ATTR_* (video_colors.h) */
} DrawPiece;

void     DrawUse(const FbInfo *fb);
/* The system's characters, drawn ahead of time (boxos_imprint.h): the one
 * copy of them the loader carries, for any hand of it that draws text. */
const BoxImprint *DrawImprint(void);
uint32_t DrawRows(void);
uint32_t DrawCols(void);
uint32_t DrawCount(const DrawPiece *pieces, uint32_t count);
uint32_t DrawPage(const DrawPiece *pieces, uint32_t count, uint32_t skip, uint32_t top, uint32_t height);
int      DrawEnd(const DrawPiece *pieces, uint32_t count);

/*
 * gangway_keys.c: the keyboard, the firmware's console's. KeyListen, at
 * Gangway's first moment, begins KEY_LISTEN_MS of listening — a held key's
 * repeat at the least. KeyPanelAsked reads every stroke waiting and answers
 * whether one was Space; with `listen`, when none was, it waits for a stroke
 * of Space through what is left of the listening — any other key waits on.
 * KeyNext waits for the next key the panel uses on the console's own event;
 * KEY_GONE when the console gives none any more.
 */
typedef enum {
    KEY_NONE,
    KEY_GONE,
    KEY_UP,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_HOME,
    KEY_END,
    KEY_ENTER,
    KEY_BACK,
} GangwayKey;

void       KeyListen(void);
int        KeyPanelAsked(int listen);
GangwayKey KeyNext(void);

/*
 * gangway_panel.c: the panel — asked for by holding Space while the machine
 * starts, after the kernel and the screen are chosen, or opened by itself
 * when `misses` starts in a row did not land (PanelIfAsked, which answers
 * CROSSING_PANEL_* — why it opened, or that it did not): a page of rows and
 * the pages they open — Start, the start's options the person may change for
 * this one start, the journals, what was found and chosen — and Restart,
 * Turn off, the firmware's settings. Leaving it, the start goes on with what
 * was changed in g_settings, and in *fb when the screen was.
 * gangway_panel_look.c: how it looks — Duppo's logo on the left, kept in the
 * loader, the page on the right, the row chosen on a plashka of his sphere —
 * drawn from rows it is handed: PanelLookBegin takes a canvas as large as the
 * screen and draws the logo and what is said under it (0: no room to draw
 * the panel), PanelLookPage draws a page and shows it, PanelLookEnd gives the
 * canvas back.
 * gangway_panel_log.c: its two journals, each shown until the person goes
 * back — this start's, the loader's own (PanelJournalShow), and what the
 * kernel said in the start before, out of its carry-over window
 * (PanelPreviousShow, boxos_logkeep.h); and the crossings kept on the medium
 * (PanelCrossingsShow).
 *
 * A page the panel draws is built of pieces in their colours: text put into
 * the page's own room (PanelPut, PanelNum), or text lying elsewhere named as
 * it is (PanelRaw) — a journal is never copied.
 */
#define PANEL_TEXT_BYTES  8192u
#define PANEL_PIECES_MOST 256u

typedef struct {
    char      text[PANEL_TEXT_BYTES];
    uint32_t  used;
    DrawPiece pieces[PANEL_PIECES_MOST];
    uint32_t  count;
} PanelPage;

void PanelBegin(PanelPage *page);
void PanelPut(PanelPage *page, uint8_t attr, const char *text);
void PanelNum(PanelPage *page, uint8_t attr, uint64_t n);
void PanelRaw(PanelPage *page, uint8_t attr, const char *text, uint32_t bytes);

typedef struct {
    const char *label;     /* NULL: a rule between groups */
    const char *value;     /* a setting's value, or what an info row says; NULL: none */
    uint8_t     more;      /* it opens a page of its own (›) */
    uint8_t     changed;   /* the value is not the one the start came with */
    uint8_t     info;      /* it says, and is never chosen */
} PanelLookRow;

int  PanelLookBegin(const FbInfo *fb, const char *said, const char *under);
void PanelLookPage(const char *title, const PanelLookRow *rows, uint32_t count, uint32_t chosen,
                   const char *note, const char *hint);
void PanelLookEnd(void);

uint8_t PanelIfAsked(FbInfo *fb, uint32_t misses);
void    PanelJournalShow(void);
void    PanelPreviousShow(void);
void    PanelCrossingsShow(void);

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
