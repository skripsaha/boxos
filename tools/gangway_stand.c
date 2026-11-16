/*
 * gangway_stand — the UEFI loader's Lectern built on the host, the very files
 * Gangway links (src/boot/uefi/gangway_lectern*.c, gangway_succession.c),
 * reading an image file the way Gangway reads its medium: the Deed at the
 * ground's head, then the kernel BOOT names and, when it is not attested, the
 * kernels the book names by role, each placed, held to its header and to the
 * digests the book keeps of it, the first attested jumped into; then the files
 * Gangway asks for by name, in its order — gangway.settings
 * (boxos_gangway_settings.h) and boot.scene (boxos_scene.h) — read and held to
 * the book as Gangway reads them.
 *
 *     build/gangway_stand [--tags "TAG ..." [--pick N]] IMAGE GROUND_START [REFUSE_BLOCK [long]]
 *
 * Every line it prints is what Gangway says of the volume, "boot: " before it
 * where Gangway puts "Gangway: ", and the same lines `volume-read IMAGE boot`
 * prints by its own code (tools/volumeread/bootread.py, bootbook.py): the two
 * are held to each other byte for byte on every landing of self-test's group
 * A (tools/gangway_stand.sh) — the agreement the shared headers alone do not
 * give, for the loops that walk the book are the loader's own (§16.7 п.2, F1).
 *
 * Where the ground is is not the Lectern's question, and Gangway answers it
 * off the partition table before the Lectern is asked (gangway_medium.c,
 * FindGroundOn): the caller says it here, as volume-read found it. Where a
 * kernel is placed is Gangway's 0x100000 and the heap here.
 *
 * With --tags, boot.scene is asked by those tags instead — as a desk's ask
 * asks it when a person's settings say so (LecternDesk.ask) — the scene that
 * --pick falls on, and `volume-read boot --tags --pick` is asked the same.
 *
 * How it read is said apart, on stderr: how many commands, the most sectors
 * one asked for (LECTERN_READ_MOST at the most), and in how many the kernel
 * jumped into came when it lies in one run. With REFUSE_BLOCK the medium
 * will not give that volume block — with `long`, only in a command of more
 * than one block, as a medium that stumbles on long commands and gives each
 * block alone: what the Lectern does when a command is refused.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boxos_gangway_settings.h"
#include "boxos_scene.h"
#include "gangway_lectern.h"
#include "settings_chosen.h"   /* SETTINGS_FILE_MAX */

typedef struct {
    uint64_t vlba;
    uint32_t sectors;
} StandCommand;

typedef struct {
    FILE         *image;
    uint64_t      start;      /* the ground's first sector */
    uint64_t      refuse;     /* the volume block it will not give; UINT64_MAX none */
    bool          long_only;  /* ... only in a command of more than one block */
    StandCommand *commands;   /* every command asked, in order */
    uint64_t      ncommands, capcommands;
} StandMedium;

static int StandRead(void *ctx, uint64_t vlba, uint32_t sectors, uint8_t *into)
{
    StandMedium *m     = (StandMedium *)ctx;
    uint64_t     first = m->refuse * VOLUME_BLOCK_SECTORS;
    if (m->ncommands == m->capcommands) {
        m->capcommands = m->capcommands ? m->capcommands * 2u : 64u;
        if (!(m->commands = realloc(m->commands, m->capcommands * sizeof(*m->commands)))) {
            perror("stand");
            exit(2);
        }
    }
    m->commands[m->ncommands++] = (StandCommand){ vlba, sectors };
    if (m->refuse != UINT64_MAX && vlba < first + VOLUME_BLOCK_SECTORS && first < vlba + sectors &&
        (!m->long_only || sectors > VOLUME_BLOCK_SECTORS))
        return 0;
    if (fseeko(m->image, (off_t)((m->start + vlba) * VOLUME_DEED_SECTOR_BYTES), SEEK_SET) != 0)
        return 0;
    return fread(into, VOLUME_DEED_SECTOR_BYTES, sectors, m->image) == sectors;
}

/* How it read, on stderr: the commands, the longest, and those that fell on
 * the kernel jumped into when it lies in one run. */
static void StandCommandsSay(const StandMedium *m, const LecternKernel *kernel)
{
    uint64_t most = 0, onkernel = 0;
    uint64_t from = kernel->first * VOLUME_BLOCK_SECTORS, to = from + kernel->blocks * VOLUME_BLOCK_SECTORS;
    for (uint64_t i = 0; i < m->ncommands; i++) {
        const StandCommand *c = &m->commands[i];
        if (c->sectors > most) most = c->sectors;
        if (c->vlba < to && from < c->vlba + c->sectors) onkernel++;
    }
    fprintf(stderr, "stand: %llu command(s), the longest %llu sector(s)", (unsigned long long)m->ncommands,
            (unsigned long long)most);
    if (kernel->image && kernel->runs == 1 && kernel->first)
        fprintf(stderr, "; the kernel's %llu block(s) from volume block %llu in %llu of them",
                (unsigned long long)kernel->blocks, (unsigned long long)kernel->first, (unsigned long long)onkernel);
    fprintf(stderr, "\n");
}

static uint8_t *StandTake(void *ctx, uint64_t bytes)
{
    (void)ctx;
    return (uint8_t *)malloc(bytes ? bytes : 1u);
}

static void StandGive(void *ctx, uint8_t *room, uint64_t bytes)
{
    (void)ctx;
    (void)bytes;
    free(room);
}

static void StandSay(void *ctx, const char *line)
{
    (void)ctx;
    printf("boot: %s\n", line);
}

/* The payload of the Deed's stamp of `kind`, its bytes in *bytes — NULL when
 * it carries none or its stamps run past it (gangway_deed.c, DeedStampOf). */
static const uint8_t *StandStamp(const uint8_t *deed, uint16_t kind, uint16_t *bytes)
{
    VolumeDeed d;
    memcpy(&d, deed, sizeof(d));
    uint32_t       left = d.stamp_bytes;
    const uint8_t *p    = deed + d.prologue_bytes;
    if (d.prologue_bytes < sizeof(VolumeDeed) || (uint32_t)d.prologue_bytes + left > VOLUME_BLOCK_BYTES)
        return NULL;
    while (left >= sizeof(VolumeStamp)) {
        VolumeStamp st;
        memcpy(&st, p, sizeof(st));
        if (st.bytes > left - sizeof(VolumeStamp))
            return NULL;
        if (st.kind == kind) {
            *bytes = st.bytes;
            return p + sizeof(VolumeStamp);
        }
        uint32_t step = (sizeof(VolumeStamp) + st.bytes + 3u) & ~3u;
        if (step > left)
            return NULL;
        p += step;
        left -= step;
    }
    return NULL;
}

/* The Deed taken up as Gangway takes it (gangway_deed.c, DeedTakeUp): the
 * desk's volume, FRAME and atom, and BOOT — *named 0 when it names no kernel.
 * 0 with nothing for the Lectern to read — a Deed with no FRAME is no book,
 * and Gangway passes its medium over — said in Gangway's words. */
static int StandDeed(const uint8_t *deed, LecternDesk *desk, VolumeBoot *boot, int *named)
{
    uint16_t       bytes = 0, frame_bytes = 0, geo_bytes = 0;
    const uint8_t *frame  = StandStamp(deed, VOLUME_STAMP_FRAME, &frame_bytes);
    const uint8_t *stamp  = StandStamp(deed, VOLUME_STAMP_BOOT, &bytes);
    const uint8_t *geo    = StandStamp(deed, VOLUME_STAMP_GEOMETRY, &geo_bytes);
    VolumeDeed     d;
    VolumeGeometry g;
    const char    *note = NULL, *why;

    memcpy(&d, deed, sizeof(d));
    if (!frame) {
        printf("boot: this deed bears no FRAME, so its volume is no book — an older one, or not TagFS's\n");
        return 0;
    }
    if ((why = VolumeFrameCheck(frame, frame_bytes, d.tail_sector, &note)) != NULL) {
        printf("boot: this deed's FRAME does not hold: %s\n", why);
        return 0;
    }
    memset(boot, 0, sizeof(*boot));
    memset(&g, 0, sizeof(g));
    if (stamp && bytes >= sizeof(VolumeBoot))
        memcpy(boot, stamp, sizeof(*boot));
    if (geo && geo_bytes >= sizeof(VolumeGeometry))
        memcpy(&g, geo, sizeof(g));
    *named = boot->kernel_block && boot->kernel_blocks;
    memcpy(&desk->frame, frame, sizeof(desk->frame));
    desk->block_end   = d.tail_sector / VOLUME_BLOCK_SECTORS;
    desk->atom_blocks = BookAtomOf(g.physical_bytes, 0, 0) / VOLUME_BLOCK_BYTES;
    return 1;
}

int main(int argc, char **argv)
{
    static uint8_t uuid[16], deed[VOLUME_BLOCK_BYTES];
    StandMedium    m;
    LecternDesk    desk;
    VolumeBoot     boot;
    LecternKernel  kernel;
    LecternCalled  called[2] = { { .name = GANGWAY_SETTINGS_FILE, .most = SETTINGS_FILE_MAX },
                                 { .name = BOX_SCENE_BOOT, .most = BOX_SCENE_FILE_MOST } };
    int            named = 0;

    static char        words[512];
    static const char *tags[64];
    uint32_t           ntags = 0;
    uint64_t           pick  = 0;
    while (argc >= 3 && (strcmp(argv[1], "--tags") == 0 || strcmp(argv[1], "--pick") == 0)) {
        if (strcmp(argv[1], "--pick") == 0) {
            pick = strtoull(argv[2], NULL, 10);
        } else {
            snprintf(words, sizeof(words), "%s", argv[2]);
            for (char *w = strtok(words, " "); w && ntags < 64u; w = strtok(NULL, " ")) tags[ntags++] = w;
        }
        argv += 2;
        argc -= 2;
    }
    if (argc < 3 || argc > 5 || (argc == 5 && strcmp(argv[4], "long") != 0)) {
        fprintf(stderr, "usage: %s [--tags \"TAG ...\" [--pick N]] IMAGE GROUND_START [REFUSE_BLOCK [long]]\n",
                argv[0]);
        return 2;
    }
    memset(&m, 0, sizeof(m));
    m.start     = strtoull(argv[2], NULL, 10);
    m.refuse    = argc >= 4 ? strtoull(argv[3], NULL, 10) : UINT64_MAX;
    m.long_only = argc == 5;
    if (!(m.image = fopen(argv[1], "rb"))) {
        perror(argv[1]);
        return 2;
    }
    if (!StandRead(&m, 0, VOLUME_BLOCK_SECTORS, deed) || memcmp(deed, "BOXDEED", 8) != 0) {
        printf("boot: unreadable — no deed at sector %llu\n", (unsigned long long)m.start);
        return 2;
    }
    memcpy(uuid, ((const VolumeDeed *)(const void *)deed)->uuid, sizeof(uuid));
    m.ncommands = 0;
    memset(&desk, 0, sizeof(desk));
    desk.ctx     = &m;
    desk.read    = StandRead;
    desk.take    = StandTake;
    desk.give    = StandGive;
    desk.say     = StandSay;
    desk.place   = StandTake;
    desk.unplace = StandGive;
    desk.uuid    = uuid;
    called[1].tags  = ntags ? tags : NULL;
    called[1].ntags = ntags;
    called[1].pick  = pick;
    if (StandDeed(deed, &desk, &boot, &named)) {
        LecternSuccession(&desk, named ? &boot : NULL, &kernel, called, 2);
        StandCommandsSay(&m, &kernel);
        if (kernel.image)
            desk.unplace(&m, kernel.image, kernel.blocks * VOLUME_BLOCK_BYTES);
        for (int k = 0; k < 2; k++)
            if (called[k].bytes) desk.give(&m, called[k].bytes, called[k].room);
    }
    free(m.commands);
    fclose(m.image);
    return 0;
}
