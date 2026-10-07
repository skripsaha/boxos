#include "tag_registry_internal.h"
#include "../mount/ground_census.h"
#include "../../../lib/kernel/crypto.h"

/*
 * The registry read off the medium at mount, what a Settle cycle's seal says
 * of it, and the ground it claims for the mount's census.
 */
/* One block's records taken in, up to `bytes` of its data, walked the way
 * every reader walks them: entry_count records packed from the start of the
 * data, none past its end. How many were taken and where they end; the
 * refusal of the record that could not be held (ERR_NO_MEMORY). */
static error_t RegistryAdoptBlock(TagRegistry* reg, const TagRegistryBlock* image, uint32_t bytes,
                                  uint16_t* out_walked, uint32_t* out_end) {
    error_t  held   = OK;
    uint16_t walked = 0;
    uint32_t end    = 0;
    if (bytes > TAGFS_REGISTRY_DATA_SIZE)
        bytes = TAGFS_REGISTRY_DATA_SIZE;
    TagFSLockTake(&reg->lock);
    while (held == OK && walked < image->entry_count && end + TAGFS_REGISTRY_RECORD_HEAD <= bytes) {
        const uint8_t* rec = image->data + end;
        uint16_t       tag_id, value_len;
        memcpy(&tag_id, rec, sizeof(tag_id));
        memcpy(&value_len, rec + 4, sizeof(value_len));
        uint32_t size = TAGFS_REGISTRY_RECORD_HEAD + rec[3] + value_len;
        if (end + size > bytes)
            break;
        held = RegistryAdopt(reg, tag_id, rec[2], rec + TAGFS_REGISTRY_RECORD_HEAD, rec[3],
                             rec + TAGFS_REGISTRY_RECORD_HEAD + rec[3], value_len);
        end += size;
        walked++;
    }
    TagFSLockDrop(&reg->lock);
    *out_walked = walked;
    *out_end    = end;
    return held;
}

/* The block a walk is at, kept for the census; false when memory ran out. */
static bool RegistryChainNote(TagRegistry* reg, uint32_t block, uint32_t* cap) {
    if (reg->chain_count == *cap) {
        uint32_t  grown = *cap ? *cap * 2u : 8u;
        uint32_t* more  = kmalloc(sizeof(uint32_t) * grown);
        if (!more)
            return false;
        if (reg->chain_count)
            memcpy(more, reg->chain_blocks, sizeof(uint32_t) * reg->chain_count);
        kfree(reg->chain_blocks);
        reg->chain_blocks = more;
        *cap              = grown;
    }
    reg->chain_blocks[reg->chain_count++] = block;
    return true;
}

/*
 * How much of the block a seal names the registry stands on: the newest
 * seal's bytes when the block bears them out, the older seal's when it names
 * the same block and the block bears those out, and none — the block's
 * records rolled back, said — when neither does.
 */
static uint32_t RegistrySealedBytes(const TagRegistryBlock* image, const SettleMark* newest,
                                    const SettleMark* older, uint32_t block) {
    const SettleMark* seal[2] = { newest, older };
    for (uint32_t i = 0; i < 2; i++) {
        const SettleMark* m = seal[i];
        if (!m || m->registry_block != block || image->magic != TAGFS_REGISTRY_MAGIC)
            continue;
        if (m->registry_bytes <= image->used_bytes && m->registry_bytes <= TAGFS_REGISTRY_DATA_SIZE &&
            KCrc32(image->data, m->registry_bytes) == m->registry_crc32)
            return m->registry_bytes;
    }
    kprintf("[TagRegistry] block %u, the registry's last, does not hold what the volume's seal "
            "says it does — a cut tore it — and its records are rolled back: the tags they "
            "named are forgotten, and their numbers are never given out again\n", block);
    return 0;
}

error_t tag_registry_load(TagRegistry* reg, uint32_t first_block, const SettleMark* newest,
                          const SettleMark* older) {
    if (!reg) return ERR_NULL_POINTER;

    /*
     * ‼ THE ONLY BOUND ON THIS WALK IS A TERMINATION PROOF, NOT A LENGTH: a
     * chain of distinct blocks is no longer than the data run it lives in, so
     * that many hops is a circle — the pool's and the table's rule. The
     * length is never layout.tag_registry_blocks, written once by mkfs.
     *
     * ‼ UNDER A SEAL IT IS READ AS FAR AS THE SEAL SAYS, AND NO FURTHER. The
     * last block is written again in place as tags are added, and the seal
     * names it with the bytes a settled cycle left and their CRC32. No record
     * under the seal can wear a tag past them; a last block that does not
     * bear them out was torn by a cut, and the registry rolls back to the
     * blocks before it. Either way the next flush writes the block whole.
     */
    TagFSState* fs  = tagfs_get_state();
    uint32_t    run = fs ? fs->layout.data_blocks : 0;
    if (run == 0) {
        kprintf("[TagRegistry] this volume states a data run of no blocks at "
                "all, so there is nowhere for a tag registry to be — it is not "
                "mounted\n");
        return ERR_TAGFS_CORRUPTED;
    }
    if (!newest) {
        newest = older;
        older  = NULL;
    }

    /* The two blocks every flush of this registry is written through, and the
     * one every block of the chain is read into on the way: the heap's, not
     * the stack's. The walk ends with the tail in tail_image. */
    reg->tail_image  = kmalloc(TAGFS_BLOCK_SIZE);
    reg->spare_image = kmalloc(TAGFS_BLOCK_SIZE);
    if (!reg->tail_image || !reg->spare_image) {
        kprintf("[TagRegistry] there is no memory for the two blocks this registry is read and "
                "written through — this volume is not mounted\n");
        return ERR_NO_MEMORY;
    }

    /* Read with no lock held: nobody can see this registry yet — the mount
     * reads it before its door opens, and a registry a test builds is the
     * test's. The lock is taken only for what goes into memory. */
    TagRegistryBlock* image  = reg->tail_image;
    uint32_t          block  = first_block;
    uint32_t          hops   = 0;
    uint32_t          cap    = 0;
    uint16_t          walked = 0;
    uint32_t          end    = 0;
    bool              owed   = false;
    for (;;) {
        if (block >= run) {
            kprintf("[TagRegistry] the chain points at block %u and this "
                    "volume's data run is %u blocks — that is outside the "
                    "volume, so it is not mounted\n", block, run);
            return ERR_TAGFS_CORRUPTED;
        }
        error_t read = tagfs_read_block(block, image);
        if (read != OK) {
            kprintf("[TagRegistry] block %u of the tag registry would not read (error %d) — this "
                    "volume is not mounted\n", block, (int)read);
            return read;
        }
        if (!RegistryChainNote(reg, block, &cap)) {
            kprintf("[TagRegistry] there is no memory to note the registry's chain at %u "
                    "blocks — this volume is not mounted\n", reg->chain_count + 1);
            return ERR_NO_MEMORY;
        }
        bool sealed_here = newest && block == newest->registry_block;
        if (!sealed_here && image->magic != TAGFS_REGISTRY_MAGIC) {
            kprintf("[TagRegistry] block %u holds 0x%08x where the tag registry should be — this "
                    "volume is not mounted\n", block, image->magic);
            return ERR_TAGFS_CORRUPTED;
        }

        uint32_t bytes = sealed_here ? RegistrySealedBytes(image, newest, older, block)
                                     : TAGFS_REGISTRY_DATA_SIZE;
        if (sealed_here && image->magic != TAGFS_REGISTRY_MAGIC) {
            memset(image, 0, TAGFS_BLOCK_SIZE);
            image->magic = TAGFS_REGISTRY_MAGIC;
        }
        error_t held = RegistryAdoptBlock(reg, image, bytes, &walked, &end);
        if (held != OK) {
            kprintf("[TagRegistry] there is no memory to hold the tags of block %u — a registry "
                    "held in part would give their numbers to new tags, so this volume is not "
                    "mounted\n", block);
            return held;
        }
        RegistryRehash(reg);

        if (sealed_here) {
            owed = image->next_block != 0 || image->used_bytes != end || image->entry_count != walked;
            break;
        }
        if (image->next_block == 0) {
            if (newest)
                kprintf("[TagRegistry] the chain ends at block %u, before block %u the volume's "
                        "seal names — it is read as it lies\n", block, newest->registry_block);
            break;
        }
        if (++hops >= run) {
            kprintf("[TagRegistry] this volume's tag registry chain has taken "
                    "%u hops in a data run of %u blocks, so it is going round "
                    "in a circle — it is not mounted\n", hops, run);
            return ERR_TAGFS_CORRUPTED;
        }
        block = image->next_block;
    }

    /* The tail as the next flush appends to it: its records end where the walk
     * ended and nothing follows them. Whatever lay past a record the walk
     * stopped at was never read by any reader, and the first append writes
     * over it. Nothing is owed but the block itself when a seal cut it: the
     * medium holds every id this registry has. */
    image->entry_count = walked;
    image->used_bytes  = (uint16_t)end;
    image->next_block  = 0;
    memset(image->data + end, 0, TAGFS_REGISTRY_DATA_SIZE - end);
    uint32_t crc = KCrc32(image->data, end);

    reg->tail_owed = owed;
    TagFSLockTake(&reg->lock);
    reg->tail_block   = block;
    reg->written_next = reg->next_id;
    reg->landed_block = block;
    reg->landed_bytes = end;
    reg->landed_crc   = crc;
    TagFSLockDrop(&reg->lock);
    return OK;
}

void TagRegistryLanded(TagRegistry* reg, SettleMark* mark) {
    TagFSLockTake(&reg->lock);
    mark->registry_block = reg->landed_block;
    mark->registry_bytes = reg->landed_bytes;
    mark->registry_crc32 = reg->landed_crc;
    TagFSLockDrop(&reg->lock);
}

/*
 * ‼ THE ID TABLE COVERS EVERY NUMBER THE REGISTRY GIVES OUT FROM.
 *
 * next_id is what every reader of an id bounds itself by — a tag's spelling,
 * the system stamp, the take-down's walk over the table — and the table under
 * it grew only when a tag was entered. Moved past it here, by the records of
 * the pool wearing ids a rolled-back registry no longer names, every one of
 * them read past the table's end: a spelling out of the heap's next block, and
 * a take-down that freed pointers nobody allocated (Z08-1). So the table grows
 * to cover the numbers before they move — made with no lock held, swapped in
 * under it — and with no memory for it the numbers do not move: the refusal is
 * said, and the mount it belongs to is refused for want of memory.
 */
error_t TagRegistryNumbersFrom(TagRegistry* reg, uint32_t next) {
    if (next > TAGFS_MAX_TAG_ID + 1u)
        next = TAGFS_MAX_TAG_ID + 1u;
    for (;;) {
        TagFSLockTake(&reg->lock);
        if (next <= reg->next_id) {
            TagFSLockDrop(&reg->lock);
            return OK;
        }
        if (next <= reg->by_id_capacity) {
            reg->next_id      = (uint16_t)next;
            reg->written_next = (uint16_t)next;
            TagFSLockDrop(&reg->lock);
            return OK;
        }
        uint32_t cap = reg->by_id_capacity;
        TagFSLockDrop(&reg->lock);

        uint32_t room = cap ? cap : 1u;
        while (room < next)
            room *= 2u;
        TagRegistryEntry** more = kmalloc(sizeof(TagRegistryEntry*) * room);
        if (!more) {
            kprintf("[TagRegistry] the volume's records wear tag ids up to %u, and there is no "
                    "memory to number the registry that far — the volume is not mounted\n",
                    next - 1u);
            return ERR_NO_MEMORY;
        }
        TagRegistryEntry** old = NULL;
        TagFSLockTake(&reg->lock);
        if (reg->by_id_capacity == cap) {
            memcpy(more, reg->by_id, sizeof(TagRegistryEntry*) * cap);
            memset(more + cap, 0, sizeof(TagRegistryEntry*) * (room - cap));
            old                 = reg->by_id;
            reg->by_id          = more;
            reg->by_id_capacity = room;
            more                = NULL;
        }
        TagFSLockDrop(&reg->lock);
        kfree(more);
        kfree(old);
    }
}

void RegistryChainLetGo(TagRegistry* reg) {
    kfree(reg->chain_blocks);
    reg->chain_blocks = NULL;
    reg->chain_count  = 0;
}

void TagRegistryClaimGround(TagRegistry* reg) {
    for (uint32_t i = 0; i < reg->chain_count; i++)
        GroundClaim(GROUND_REGISTRY, reg->chain_blocks[i], 1, 0);
    RegistryChainLetGo(reg);
}
