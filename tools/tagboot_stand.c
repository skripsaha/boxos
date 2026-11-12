/*
 * tagboot_stand — the UEFI loader's Lectern built on the host, the very file
 * TagBoot links (src/boot/uefi/tagboot_lectern.c), reading an image file the
 * way TagBoot reads its medium: the Deed at the ground's head, the kernel BOOT
 * names loaded block by block, and the book asked whether those blocks are
 * the ones its Tally keeps the digests of (spec_v2 §16.7 пп.2, 4, 6).
 *
 *     build/tagboot_stand IMAGE GROUND_START
 *
 * Every line it prints is what TagBoot says of the volume, "boot: " before it
 * where TagBoot puts "TagBoot: ", and the same lines `volume-read IMAGE boot`
 * prints by its own code (tools/volumeread/bootread.py): the two are held to
 * each other byte for byte on every landing of self-test's group A
 * (tools/tagboot_stand.sh) — the agreement the shared headers alone do not
 * give, for the loop that walks the book is the loader's own (§16.7 п.2, F1).
 *
 * Where the ground is is not the Lectern's question, and TagBoot answers it
 * off the partition table before the Lectern is asked (tagboot.c,
 * FindGroundOn): the caller says it here, as volume-read found it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tagboot_lectern.h"

#define WHO "the kernel the deed names"

typedef struct {
    FILE    *image;
    uint64_t start;           /* the ground's first sector */
} StandMedium;

static int StandRead(void *ctx, uint64_t vlba, uint32_t sectors, uint8_t *into)
{
    StandMedium *m = (StandMedium *)ctx;
    if (fseeko(m->image, (off_t)((m->start + vlba) * VOLUME_DEED_SECTOR_BYTES), SEEK_SET) != 0)
        return 0;
    return fread(into, VOLUME_DEED_SECTOR_BYTES, sectors, m->image) == sectors;
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
 * it carries none or its stamps run past it (tagboot_deed.c, DeedStampOf). */
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

/* The kernel's blocks, loaded as TagBoot loads them (tagboot.c,
 * TagFsLoadKernel): as many as its bytes take, every one BOOT names when it
 * names no bytes. 0 when its bytes overrun its blocks, said. */
static uint64_t StandLoaded(const VolumeBoot *boot)
{
    uint64_t have = (uint64_t)boot->kernel_blocks * VOLUME_BLOCK_BYTES;
    if (boot->kernel_bytes > have) {
        printf("boot: the kernel is said to be %u bytes and its blocks hold %llu\n", boot->kernel_bytes,
               (unsigned long long)have);
        return 0;
    }
    if (boot->kernel_bytes == 0)
        return boot->kernel_blocks;
    return (boot->kernel_bytes + VOLUME_BLOCK_BYTES - 1u) / VOLUME_BLOCK_BYTES;
}

/* The Deed taken up as TagBoot takes it (tagboot_deed.c, DeedTakeUp): the
 * desk's volume and FRAME, and BOOT. 0 with nothing for the Lectern to read,
 * said in TagBoot's words. */
static int StandDeed(const uint8_t *deed, LecternDesk *desk, VolumeBoot *boot)
{
    uint16_t       bytes = 0, frame_bytes = 0;
    const uint8_t *layout = StandStamp(deed, VOLUME_STAMP_LAYOUT, &bytes);
    const uint8_t *frame  = StandStamp(deed, VOLUME_STAMP_FRAME, &frame_bytes);
    const uint8_t *stamp  = StandStamp(deed, VOLUME_STAMP_BOOT, &bytes);
    VolumeDeed     d;
    const char    *note = NULL, *why;

    memcpy(&d, deed, sizeof(d));
    if (!frame) {
        printf("boot: a format-v%s volume — the loader takes its kernel as it always has, held to no book\n",
               layout ? "1" : "-");
        return 0;
    }
    if ((why = VolumeFrameCheck(frame, frame_bytes, d.tail_sector, &note)) != NULL) {
        printf("boot: this deed's FRAME does not hold: %s\n", why);
        return 0;
    }
    memset(boot, 0, sizeof(*boot));
    if (stamp && bytes >= sizeof(VolumeBoot))
        memcpy(boot, stamp, sizeof(*boot));
    if (!boot->kernel_block || !boot->kernel_blocks) {
        printf("boot: this volume's deed names no kernel\n");
        return 0;
    }
    memcpy(&desk->frame, frame, sizeof(desk->frame));
    desk->block_end = d.tail_sector / VOLUME_BLOCK_SECTORS;
    return 1;
}

int main(int argc, char **argv)
{
    static uint8_t uuid[16], deed[VOLUME_BLOCK_BYTES];
    StandMedium    m;
    LecternDesk    desk;
    VolumeBoot     boot;
    AttestVerdict  verdict;

    if (argc != 3) {
        fprintf(stderr, "usage: %s IMAGE GROUND_START\n", argv[0]);
        return 2;
    }
    m.start = strtoull(argv[2], NULL, 10);
    if (!(m.image = fopen(argv[1], "rb"))) {
        perror(argv[1]);
        return 2;
    }
    if (!StandRead(&m, 0, VOLUME_BLOCK_SECTORS, deed) || memcmp(deed, "BOXDEED", 8) != 0) {
        printf("boot: unreadable — no deed at sector %llu\n", (unsigned long long)m.start);
        return 2;
    }
    memcpy(uuid, ((const VolumeDeed *)(const void *)deed)->uuid, sizeof(uuid));
    desk.ctx  = &m;
    desk.read = StandRead;
    desk.take = StandTake;
    desk.give = StandGive;
    desk.say  = StandSay;
    desk.uuid = uuid;
    if (!StandDeed(deed, &desk, &boot))
        return 0;
    uint64_t blocks = StandLoaded(&boot);
    if (!blocks)
        return 0;
    uint8_t *image = (uint8_t *)malloc(blocks * VOLUME_BLOCK_BYTES);
    if (!image || !StandRead(&m, (uint64_t)boot.kernel_block * VOLUME_BLOCK_SECTORS,
                             (uint32_t)(blocks * VOLUME_BLOCK_SECTORS), image)) {
        printf("boot: the kernel's blocks will not load\n");
        return 2;
    }
    printf("boot: the deed names the kernel at volume block %u, %llu block(s) for its %u byte(s)\n",
           boot.kernel_block, (unsigned long long)blocks, boot.kernel_bytes);
    LecternAttest(&desk, WHO, boot.kernel_block, blocks, image, &verdict);
    free(image);
    fclose(m.image);
    return 0;
}
