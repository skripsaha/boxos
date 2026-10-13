#include "tests.h"
#include "../tagfs.h"
#include "../deed/deed.h"
#include "deed_pen.h"

#ifdef CONFIG_VOLUME_TESTS

// ============================================================================
// FRAME — which format a volume's book is in, and whether its frame holds
//
// The mount asks both before it takes anything from a volume (tagfs.c,
// volume_take_ground), and the answers decide whether a byte of it may be
// written: a format-v2 volume, which this kernel cannot read, must be turned
// away whole. The Deeds here are written in memory with the one pen mkfs and
// the kernel share, and read with the reader's own walk — nothing touches a
// medium.
// ============================================================================

/* What mkfs lays down (tools/create_tagfs_v2.c, --frame-only): the ring a grain
 * apart from the grain after the Deed's, the data run from the grain after the
 * ring's last slot — in 4 KiB blocks of a 1 MiB grain. */
#define FRAME_GRAIN_BLOCKS  256u
#define FRAME_TAIL_BLOCK    6000u     /* where the far copy sits: the volume's last block */

static VolumeFrame FrameAsMade(void)
{
    VolumeFrame f;
    f.spine_block  = FRAME_GRAIN_BLOCKS;
    f.spine_stride = FRAME_GRAIN_BLOCKS;
    f.spine_slots  = VOLUME_FRAME_SPINE_SLOTS;
    f.data_block   = (uint64_t)FRAME_GRAIN_BLOCKS * (1u + VOLUME_FRAME_SPINE_SLOTS);
    return f;
}

/* Which stamps a Deed in a test carries. */
#define FRAME_SAYS_LAYOUT  (1u << 0)
#define FRAME_SAYS_FRAME   (1u << 1)

/*
 * A Deed in g_test_buffer, sealed, carrying GEOMETRY and BOOT and whichever of
 * LAYOUT and FRAME `says` names — the FRAME `frame_bytes` long, padded with
 * zeros past the struct — and `copy` set to read it as DeedReadHead would
 * hand it over. The copy's bytes are the test buffer's: it is never released.
 */
static bool FrameDeedWrite(DeedCopy *copy, uint32_t says, const VolumeFrame *frame,
                           uint16_t frame_bytes)
{
    uint8_t *block = g_test_buffer;
    uint8_t  payload[sizeof(VolumeFrame) + 8];

    if (frame_bytes > sizeof(payload))
        return false;
    memset(payload, 0, sizeof(payload));
    memcpy(payload, frame, frame_bytes < sizeof(*frame) ? frame_bytes : sizeof(*frame));

    VolumeGeometry geo   = { .logical_bytes = VOLUME_DEED_SECTOR_BYTES,
                             .physical_bytes = VOLUME_BLOCK_BYTES,
                             .grain_bytes = FRAME_GRAIN_BLOCKS * VOLUME_BLOCK_BYTES,
                             .block_bytes = VOLUME_BLOCK_BYTES };
    VolumeLayout   layout;
    VolumeBoot     boot  = { .kernel_block = (uint32_t)frame->data_block,
                             .kernel_blocks = 1, .kernel_bytes = 1, .reserved = 0 };
    memset(&layout, 0, sizeof(layout));
    layout.total_blocks = FRAME_TAIL_BLOCK + 1u;
    layout.data_block   = 700u;
    layout.data_blocks  = FRAME_TAIL_BLOCK - 700u;

    DeedPen pen;
    DeedPenOpen(&pen, block, DEED_PEN_BYTES);
    if (DeedPenSay(&pen, VOLUME_STAMP_GEOMETRY, &geo, sizeof(geo)) != 0)
        return false;
    if ((says & FRAME_SAYS_LAYOUT) &&
        DeedPenSay(&pen, VOLUME_STAMP_LAYOUT, &layout, sizeof(layout)) != 0)
        return false;
    if ((says & FRAME_SAYS_FRAME) &&
        DeedPenSay(&pen, VOLUME_STAMP_FRAME, payload, frame_bytes) != 0)
        return false;
    if (DeedPenSay(&pen, VOLUME_STAMP_BOOT, &boot, sizeof(boot)) != 0)
        return false;
    DeedPenSetGround(&pen, (uint64_t)(FRAME_TAIL_BLOCK + 1u) * VOLUME_BLOCK_SECTORS,
                     (uint64_t)FRAME_TAIL_BLOCK * VOLUME_BLOCK_SECTORS);
    DeedPenSetRole(&pen, VOLUME_DEED_ROLE_HEAD);
    DeedPenSeal(&pen);

    memset(copy, 0, sizeof(*copy));
    memcpy(&copy->head, block, sizeof(copy->head));
    copy->raw       = block;
    copy->raw_bytes = DEED_PEN_BYTES;
    copy->stamps    = block + copy->head.prologue_bytes;
    return true;
}

/* GEOMETRY + FRAME + BOOT is format v2, + LAYOUT instead is v1, both is a
 * Deed claiming two formats, neither is one that says where nothing is. */
static TestResult TestFrameDispatch(void)
{
    TestResult  verdict = TEST_PASS;
    DeedCopy    copy;
    VolumeFrame made = FrameAsMade();

    TEST_ASSERT(FrameDeedWrite(&copy, FRAME_SAYS_FRAME, &made, sizeof(made)),
                "a Deed with a FRAME is written in memory");
    TEST_ASSERT_EQ(DeedFormatOf(&copy), DEED_FORMAT_V2, "GEOMETRY, FRAME and BOOT read as format v2");

    TEST_ASSERT(FrameDeedWrite(&copy, FRAME_SAYS_LAYOUT, &made, sizeof(made)),
                "a Deed with a LAYOUT is written in memory");
    TEST_ASSERT_EQ(DeedFormatOf(&copy), DEED_FORMAT_V1, "GEOMETRY, LAYOUT and BOOT read as format v1");

    TEST_ASSERT(FrameDeedWrite(&copy, FRAME_SAYS_LAYOUT | FRAME_SAYS_FRAME, &made, sizeof(made)),
                "a Deed with both is written in memory");
    TEST_ASSERT_EQ(DeedFormatOf(&copy), DEED_FORMAT_BOTH, "a LAYOUT and a FRAME read as both");

    TEST_ASSERT(FrameDeedWrite(&copy, 0, &made, sizeof(made)),
                "a Deed with neither is written in memory");
    TEST_ASSERT_EQ(DeedFormatOf(&copy), DEED_FORMAT_NONE, "GEOMETRY and BOOT alone read as no format");
out:
    return verdict;
}

/* The FRAME as `copy` carries it, put through the check. */
static const char *FrameJudged(const DeedCopy *copy, const char **note)
{
    uint16_t    bytes = 0;
    const void *frame = DeedStamp(copy, VOLUME_STAMP_FRAME, &bytes);
    return VolumeFrameCheck(frame, bytes, copy->head.tail_sector, note);
}

/* The FRAME `frame`, `frame_bytes` long, in a Deed of format v2 — and whether
 * it holds and has something to say, as the check answers. */
static bool FrameHolds(const VolumeFrame *frame, uint16_t frame_bytes, bool *noted)
{
    DeedCopy    copy;
    const char *note = NULL;
    if (!FrameDeedWrite(&copy, FRAME_SAYS_FRAME, frame, frame_bytes)) {
        *noted = false;
        return false;
    }
    bool holds = FrameJudged(&copy, &note) == NULL;
    *noted = note != NULL;
    return holds;
}

static TestResult TestFrameCheck(void)
{
    TestResult  verdict = TEST_PASS;
    VolumeFrame f;
    bool        noted = false;

    f = FrameAsMade();
    TEST_ASSERT(FrameHolds(&f, sizeof(f), &noted) && !noted,
                "the FRAME mkfs lays down holds, with nothing to say about it");

    f = FrameAsMade(); f.spine_slots = 1;
    TEST_ASSERT(!FrameHolds(&f, sizeof(f), &noted), "a ring of one slot is refused");

    f = FrameAsMade(); f.spine_slots = 2;
    TEST_ASSERT(FrameHolds(&f, sizeof(f), &noted) && noted,
                "a ring of two holds, and is named as another maker's");

    f = FrameAsMade(); f.spine_stride = 0;
    TEST_ASSERT(!FrameHolds(&f, sizeof(f), &noted), "a ring with no stride is refused");

    f = FrameAsMade(); f.spine_block = 0;
    TEST_ASSERT(!FrameHolds(&f, sizeof(f), &noted), "a ring on the Deed's own block is refused");

    f = FrameAsMade(); f.spine_slots = VOLUME_FRAME_SPINE_SLOTS + 1u;
    TEST_ASSERT(!FrameHolds(&f, sizeof(f), &noted),
                "a slot on the data run's first block is refused");

    /* The one way left for slots to land together: a ring that runs past the
     * last block number and comes round onto the blocks before it. */
    f = FrameAsMade(); f.spine_block = UINT64_MAX - FRAME_GRAIN_BLOCKS + 1u;
    TEST_ASSERT(!FrameHolds(&f, sizeof(f), &noted),
                "a ring that comes round past the last block number is refused");

    f = FrameAsMade(); f.data_block = FRAME_TAIL_BLOCK;
    TEST_ASSERT(!FrameHolds(&f, sizeof(f), &noted),
                "a data run starting on the far copy's block is refused");

    f = FrameAsMade();
    TEST_ASSERT(!FrameHolds(&f, sizeof(f) - 4u, &noted), "a FRAME of 20 bytes is refused");

    f = FrameAsMade();
    TEST_ASSERT(FrameHolds(&f, sizeof(f) + 8u, &noted) && noted,
                "a FRAME of 32 bytes holds, and is named as written by a kernel that knows more");
out:
    return verdict;
}

static const TestCase g_frame_tests[] = {
    { .name = "frame_dispatch", .func = TestFrameDispatch, .reach = TEST_READS_ONLY },
    { .name = "frame_check",    .func = TestFrameCheck,    .reach = TEST_READS_ONLY },
};

const TestSuite g_frame_suite = {
    .tests = g_frame_tests, .count = ARRAY_SIZE(g_frame_tests),
};

#endif /* CONFIG_VOLUME_TESTS */
