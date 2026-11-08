/*
 * clipboard.c — the machine's list of what was copied (see clipboard.h).
 *
 * Lock ordering: g_clipboard.lock is a leaf. Under it only links, counts and
 * terms are read and written; allocation, copying, wiping, freeing and every
 * Touch happen outside it.
 */

#include "clipboard.h"
#include "cabin.h"
#include "klib.h"
#include "pit.h"
#include "touch.h"

/* A clip's bytes are held in leaves of this many bytes, the last holding what
 * is left: a clip of any length is pieces the heap can always give, never
 * one stretch it has to find whole. */
#define CLIP_LEAF (64u * 1024u)

typedef struct Clip Clip;

struct Clip {
    Clip    *newer;              /* the list's links, under the lock; once the  */
    Clip    *older;              /* clip is unlinked, `older` chains a batch    */
    uint32_t refs;               /* the list's one while linked, a taker's each */
    uint32_t takes_left;         /* 0: no such term                             */
    uint32_t flags;              /* CLIPBOARD_WHILE_ALIVE                       */
    uint32_t holder_pid;         /* the program a WHILE_ALIVE clip lives with   */
    uint32_t holder_generation;
    uint64_t number;
    uint64_t bytes;
    uint64_t put_us;
    uint32_t leaf_count;         /* leaves allocated so far                     */
    uint8_t *leaves[];
};

static struct {
    spinlock_t lock;
    Clip      *newest;
    Clip      *oldest;
    uint64_t   last_number;
    uint64_t   clips;
    uint64_t   bytes;
    uint64_t   while_alive;      /* linked clips put WHILE_ALIVE */
} g_clipboard = {0};

static uint64_t LeafLength(const Clip *clip, uint32_t leaf)
{
    const uint64_t from = (uint64_t)leaf * CLIP_LEAF;
    return clip->bytes - from < CLIP_LEAF ? clip->bytes - from : CLIP_LEAF;
}

/* Zeros a store that is never read again would be free to drop; the barrier
 * says the memory is read, so they are written. */
static void Wipe(void *bytes, uint64_t n)
{
    memset(bytes, 0, (size_t)n);
    __asm__ __volatile__("" : : "r"(bytes) : "memory");
}

static void ClipFree(Clip *clip)
{
    for (uint32_t leaf = 0; leaf < clip->leaf_count; leaf++) {
        Wipe(clip->leaves[leaf], LeafLength(clip, leaf));
        kfree(clip->leaves[leaf]);
    }
    kfree(clip);
}

static void ClipHold(Clip *clip)
{
    __atomic_add_fetch(&clip->refs, 1, __ATOMIC_RELAXED);
}

static void ClipRelease(Clip *clip)
{
    if (__atomic_sub_fetch(&clip->refs, 1, __ATOMIC_ACQ_REL) == 0)
        ClipFree(clip);
}

static void Say(uint32_t what, uint64_t number)
{
    ClipboardChange change = { number, what, 0 };
    TouchPublish(CLIPBOARD_TOUCH_TAG, &change, sizeof(change));
}

/* Under the lock: the newest clip numbered `number` or below. */
static Clip *AtMost(uint64_t number)
{
    Clip *clip = g_clipboard.newest;
    while (clip && clip->number > number)
        clip = clip->older;
    return clip;
}

/* Under the lock: the clip numbered `number`, or NULL. */
static Clip *Find(uint64_t number)
{
    Clip *clip = AtMost(number);
    return clip && clip->number == number ? clip : NULL;
}

/* Under the lock: the clip out of the list. The list's reference passes to
 * the caller, who lets go of it outside the lock. */
static void Unlink(Clip *clip)
{
    if (clip->newer) clip->newer->older = clip->older;
    else             g_clipboard.newest = clip->older;
    if (clip->older) clip->older->newer = clip->newer;
    else             g_clipboard.oldest = clip->newer;
    clip->newer = NULL;
    clip->older = NULL;

    g_clipboard.clips--;
    g_clipboard.bytes -= clip->bytes;
    if (clip->flags & CLIPBOARD_WHILE_ALIVE) g_clipboard.while_alive--;
}

error_t ClipboardPut(uint64_t bytes, ClipFill fill, void *source, uint32_t takes, uint32_t flags,
                     const cabin_t *putter, uint64_t *number)
{
    if (bytes == 0 || !fill || !number)           return ERR_INVALID_ARGUMENT;
    if (flags & ~CLIPBOARD_WHILE_ALIVE)           return ERR_INVALID_ARGUMENT;

    uint32_t pid = 0, generation = 0;
    if ((flags & CLIPBOARD_WHILE_ALIVE) && !CabinProgram(putter, &pid, &generation))
        return ERR_INVALID_ARGUMENT;

    /* A clip longer than the leaf table can count could never be held. */
    const uint64_t leaves = (bytes + CLIP_LEAF - 1) / CLIP_LEAF;
    if (leaves > UINT32_MAX) return ERR_NO_MEMORY;

    Clip *clip = kmalloc(sizeof(Clip) + (size_t)leaves * sizeof(uint8_t *));
    if (!clip) return ERR_NO_MEMORY;
    memset(clip, 0, sizeof(Clip));
    clip->refs              = 1;
    clip->takes_left        = takes;
    clip->flags             = flags;
    clip->holder_pid        = pid;
    clip->holder_generation = generation;
    clip->bytes             = bytes;

    for (uint32_t leaf = 0; leaf < (uint32_t)leaves; leaf++) {
        const uint64_t length = LeafLength(clip, leaf);
        uint8_t       *into   = kmalloc((size_t)length);
        if (!into) {
            ClipFree(clip);
            return ERR_NO_MEMORY;
        }
        clip->leaves[clip->leaf_count++] = into;
        error_t filled = fill(source, (uint64_t)leaf * CLIP_LEAF, into, length);
        if (filled != OK) {
            ClipFree(clip);
            return filled;
        }
    }
    clip->put_us = pit_get_uptime_us();

    spin_lock(&g_clipboard.lock);
    clip->number = ++g_clipboard.last_number;
    clip->older  = g_clipboard.newest;
    if (g_clipboard.newest) g_clipboard.newest->newer = clip;
    else                    g_clipboard.oldest        = clip;
    g_clipboard.newest = clip;
    g_clipboard.clips++;
    g_clipboard.bytes += bytes;
    if (flags & CLIPBOARD_WHILE_ALIVE) g_clipboard.while_alive++;
    *number = clip->number;
    spin_unlock(&g_clipboard.lock);

    Say(CLIPBOARD_PUT, *number);
    return OK;
}

uint32_t ClipboardList(uint64_t from, ClipboardRecord *out, uint32_t room, uint64_t *next, uint64_t *clips,
                       uint64_t *bytes)
{
    uint32_t written = 0;

    spin_lock(&g_clipboard.lock);
    Clip *clip = from == 0 ? g_clipboard.newest : AtMost(from);
    for (; clip && written < room; clip = clip->older) {
        out[written].number     = clip->number;
        out[written].bytes      = clip->bytes;
        out[written].put_us     = clip->put_us;
        out[written].takes_left = clip->takes_left;
        out[written].flags      = clip->flags;
        written++;
    }
    *next  = clip ? clip->number : 0;
    *clips = g_clipboard.clips;
    *bytes = g_clipboard.bytes;
    spin_unlock(&g_clipboard.lock);

    return written;
}

error_t ClipboardTake(uint64_t number, uint64_t offset, uint64_t room, ClipPour pour, void *sink,
                      uint64_t *given)
{
    if (!pour || !given || room == 0) return ERR_INVALID_ARGUMENT;

    spin_lock(&g_clipboard.lock);
    Clip *clip = Find(number);
    if (!clip) {
        spin_unlock(&g_clipboard.lock);
        return ERR_OBJECT_NOT_FOUND;
    }
    if (offset >= clip->bytes) {
        spin_unlock(&g_clipboard.lock);
        return ERR_INVALID_ARGUMENT;
    }
    const uint64_t n = room < clip->bytes - offset ? room : clip->bytes - offset;

    /* A clip with takes left: whole or not at all, and the take is spent
     * here, under the hold that found it. Spending the last one unlinks the
     * clip, and the list's reference becomes this take's. */
    bool spent = false;
    if (clip->takes_left != 0) {
        if (offset != 0 || n != clip->bytes) {
            spin_unlock(&g_clipboard.lock);
            return ERR_BUFFER_TOO_SMALL;
        }
        if (--clip->takes_left == 0) {
            Unlink(clip);
            spent = true;
        }
    }
    if (!spent) ClipHold(clip);
    spin_unlock(&g_clipboard.lock);

    error_t  poured = OK;
    uint64_t done   = 0;
    for (uint32_t leaf = (uint32_t)(offset / CLIP_LEAF); done < n && poured == OK; leaf++) {
        const uint64_t within = done == 0 ? offset % CLIP_LEAF : 0;
        uint64_t       length = LeafLength(clip, leaf) - within;
        if (length > n - done) length = n - done;
        poured = pour(sink, done, clip->leaves[leaf] + within, length);
        done += length;
    }

    ClipRelease(clip);
    if (spent) Say(CLIPBOARD_GONE, number);
    if (poured != OK) return poured;
    *given = n;
    return OK;
}

error_t ClipboardDrop(uint64_t number)
{
    spin_lock(&g_clipboard.lock);
    Clip *clip = Find(number);
    if (clip) Unlink(clip);
    spin_unlock(&g_clipboard.lock);

    if (!clip) return ERR_OBJECT_NOT_FOUND;
    ClipRelease(clip);
    Say(CLIPBOARD_GONE, number);
    return OK;
}

uint64_t ClipboardClear(void)
{
    spin_lock(&g_clipboard.lock);
    Clip    *all   = g_clipboard.newest;
    uint64_t count = g_clipboard.clips;
    g_clipboard.newest      = NULL;
    g_clipboard.oldest      = NULL;
    g_clipboard.clips       = 0;
    g_clipboard.bytes       = 0;
    g_clipboard.while_alive = 0;
    spin_unlock(&g_clipboard.lock);

    while (all) {
        Clip *older = all->older;
        ClipRelease(all);
        all = older;
    }
    if (count != 0) Say(CLIPBOARD_CLEARED, 0);
    return count;
}

void ClipboardCleanupCabin(const cabin_t *cabin)
{
    if (__atomic_load_n(&g_clipboard.while_alive, __ATOMIC_RELAXED) == 0) return;

    uint32_t pid, generation;
    if (!CabinProgram(cabin, &pid, &generation)) return;

    Clip *gone = NULL;
    spin_lock(&g_clipboard.lock);
    Clip *clip = g_clipboard.newest;
    while (clip) {
        Clip *older = clip->older;
        if ((clip->flags & CLIPBOARD_WHILE_ALIVE) && clip->holder_pid == pid &&
            clip->holder_generation == generation) {
            Unlink(clip);
            clip->older = gone;
            gone        = clip;
        }
        clip = older;
    }
    spin_unlock(&g_clipboard.lock);

    while (gone) {
        Clip          *older  = gone->older;
        const uint64_t number = gone->number;
        ClipRelease(gone);
        Say(CLIPBOARD_GONE, number);
        gone = older;
    }
}
