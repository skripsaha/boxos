/*
 * FAT32 read and written by one code for the host, the kernel and Gangway —
 * see src/include/fat.h. The layout is Microsoft's "FAT: General Overview of
 * On-Disk Format" (fatgen103) and the long names its "Long File Name
 * Specification"; nothing here calls a C library.
 */

#if __STDC_HOSTED__
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#elif defined(BOXOS_UEFI)
#include "uefi.h"
#else
#include "ktypes.h"
#endif
#include "fat.h"

#define FAT_EOC        0x0FFFFFF8u   /* at or above: the chain ends */
#define FAT_BAD        0x0FFFFFF7u
#define FAT_MASK       0x0FFFFFFFu
#define FAT_RESERVED   32u           /* sectors before the first FAT a FAT32 is laid with, at least */
#define FAT_CLUSTERS_MOST 0x0FFFFFF4u /* data clusters 2 to 0x0FFFFFF5: past them the FAT's own values */
#define FAT_SLOT       32u           /* a directory entry */
#define FAT_DELETED    0xE5u
#define FAT_LAST_LONG  0x40u         /* the first slot of a long name: its last part */
#define FAT_CASE_BASE  0x08u         /* the short name's base is lower case (Windows NT) */
#define FAT_CASE_EXT   0x10u         /* and its extension */
#define FAT_LONG_CHARS 13u           /* characters a long-name slot holds */

// ----------------------------------------------------------------------------
// Bytes
// ----------------------------------------------------------------------------

static uint16_t Get16(const uint8_t* p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t Get32(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static void Put16(uint8_t* p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void Put32(uint8_t* p, uint32_t v)
{
    Put16(p, v);
    Put16(p + 2, v >> 16);
}

static void Fill(uint8_t* p, uint8_t b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        p[i] = b;
}

static void Copy(uint8_t* to, const uint8_t* from, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        to[i] = from[i];
}

static uint8_t Upper(uint8_t c) { return c >= 'a' && c <= 'z' ? (uint8_t)(c - 32) : c; }
static int     IsLower(uint8_t c) { return c >= 'a' && c <= 'z'; }
static int     IsUpper(uint8_t c) { return c >= 'A' && c <= 'Z'; }

// ----------------------------------------------------------------------------
// The medium, the FAT and the clusters
// ----------------------------------------------------------------------------

static int Read(FatVolume* v, uint64_t sector, uint32_t count, void* into)
{
    return v->medium->read(v->medium->ctx, sector, count, into) == 0 ? FAT_OK : FAT_IO;
}

static int Write(FatVolume* v, uint64_t sector, uint32_t count, const void* from)
{
    return v->medium->write(v->medium->ctx, sector, count, from) == 0 ? FAT_OK : FAT_IO;
}

static uint64_t ClusterSector(const FatVolume* v, uint32_t c)
{
    return v->data_start + (uint64_t)(c - 2u) * v->cluster_sectors;
}

static int ClusterValid(const FatVolume* v, uint32_t c)
{
    return c >= 2u && c < v->clusters + 2u;
}

/* fat_buf written back to every FAT, when it changed. */
static int FatSync(FatVolume* v)
{
    if (!v->fat_dirty)
        return FAT_OK;
    for (uint32_t i = 0; i < v->fats; i++)
        if (Write(v, v->reserved + (uint64_t)i * v->fat_sectors + v->fat_at, 1, v->fat_buf) != FAT_OK)
            return FAT_IO;
    v->fat_dirty = 0;
    return FAT_OK;
}

/* The FAT's sector `s` in fat_buf. */
static int FatSector(FatVolume* v, uint64_t s)
{
    if (v->fat_at == s)
        return FAT_OK;
    if (FatSync(v) != FAT_OK || Read(v, v->reserved + s, 1, v->fat_buf) != FAT_OK) {
        v->fat_at = ~(uint64_t)0;
        return FAT_IO;
    }
    v->fat_at = s;
    return FAT_OK;
}

static int FatGet(FatVolume* v, uint32_t c, uint32_t* out)
{
    uint64_t at = (uint64_t)c * 4u;
    if (FatSector(v, at / v->sector_bytes) != FAT_OK)
        return FAT_IO;
    *out = Get32(v->fat_buf + at % v->sector_bytes) & FAT_MASK;
    return FAT_OK;
}

/* Cluster `c`'s entry set to `value`, its top four bits as they were. */
static int FatSet(FatVolume* v, uint32_t c, uint32_t value)
{
    uint64_t at = (uint64_t)c * 4u;
    if (FatSector(v, at / v->sector_bytes) != FAT_OK)
        return FAT_IO;
    uint8_t* p = v->fat_buf + at % v->sector_bytes;
    Put32(p, (Get32(p) & ~FAT_MASK) | (value & FAT_MASK));
    v->fat_dirty = 1;
    return FAT_OK;
}

/* The cluster after `c` in its chain: *next a cluster, or at least FAT_EOC
 * at its end. FAT_DAMAGED for a free, bad or out-of-range one. */
static int ChainNext(FatVolume* v, uint32_t c, uint32_t* next)
{
    if (FatGet(v, c, next) != FAT_OK)
        return FAT_IO;
    if (*next >= FAT_EOC)
        return FAT_OK;
    return ClusterValid(v, *next) ? FAT_OK : FAT_DAMAGED;
}

/* What is known of the free clusters written into FSInfo. */
static int FsInfoWrite(FatVolume* v)
{
    if (!v->fsinfo)
        return FAT_OK;
    if (Read(v, v->fsinfo, 1, v->buf) != FAT_OK)
        return FAT_IO;
    if (Get32(v->buf) != 0x41615252u || Get32(v->buf + 484) != 0x61417272u)
        return FAT_OK;      /* not one this code trusts: left as it is */
    Put32(v->buf + 488, v->free_count < 0 ? 0xFFFFFFFFu : (uint32_t)v->free_count);
    Put32(v->buf + 492, v->free_hint);
    return Write(v, v->fsinfo, 1, v->buf);
}

/* Everything changed so far on the medium: the FATs, FSInfo, then a flush. */
static int Commit(FatVolume* v)
{
    if (FatSync(v) != FAT_OK || FsInfoWrite(v) != FAT_OK)
        return FAT_IO;
    return v->medium->flush(v->medium->ctx) == 0 ? FAT_OK : FAT_IO;
}

/* `n` free clusters in one run, chained in order and ended, into *first —
 * the search from the hint on, round to it. FAT_NO_ROOM when no run is long
 * enough. */
static int RunTaken(FatVolume* v, uint32_t n, uint32_t* first)
{
    uint32_t start = ClusterValid(v, v->free_hint) ? v->free_hint : 2u;
    uint32_t run = 0, at = 0;
    for (uint32_t i = 0; i < v->clusters; i++) {
        uint32_t c = 2u + (start - 2u + i) % v->clusters, entry;
        if (c == 2u)
            run = 0;        /* round past the end: no run goes on from it */
        if (FatGet(v, c, &entry) != FAT_OK)
            return FAT_IO;
        if (entry != 0) {
            run = 0;
            continue;
        }
        if (run++ == 0)
            at = c;
        if (run < n)
            continue;
        for (uint32_t j = 0; j < n; j++)
            if (FatSet(v, at + j, j + 1u == n ? 0x0FFFFFFFu : at + j + 1u) != FAT_OK)
                return FAT_IO;
        v->free_hint = ClusterValid(v, at + n) ? at + n : 2u;
        if (v->free_count >= 0)
            v->free_count = v->free_count >= (int64_t)n ? v->free_count - n : -1;
        *first = at;
        return FAT_OK;
    }
    return FAT_NO_ROOM;
}

/* Every cluster of the chain from `c` given back. */
static int ChainFreed(FatVolume* v, uint32_t c)
{
    for (uint32_t guard = 0; ClusterValid(v, c); guard++) {
        uint32_t next;
        if (guard > v->clusters)
            return FAT_DAMAGED;
        if (FatGet(v, c, &next) != FAT_OK || FatSet(v, c, 0) != FAT_OK)
            return FAT_IO;
        if (v->free_count >= 0)
            v->free_count++;
        if (next >= FAT_EOC || next == 0)
            break;
        c = next;
    }
    return FAT_OK;
}

/* Cluster `c` all zeros on the medium. */
static int ClusterZeroed(FatVolume* v, uint32_t c)
{
    Fill(v->buf, 0, v->sector_bytes);
    for (uint32_t s = 0; s < v->cluster_sectors; s++)
        if (Write(v, ClusterSector(v, c) + s, 1, v->buf) != FAT_OK)
            return FAT_IO;
    return FAT_OK;
}

// ----------------------------------------------------------------------------
// Names
// ----------------------------------------------------------------------------

/* Whether `c` may stand in a short name. */
static int ShortChar(uint8_t c)
{
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c >= 0x80)
        return c < 0x80;
    for (const char* ok = "!#$%&'()-@^_`{}~"; *ok; ok++)
        if ((uint8_t)*ok == c)
            return 1;
    return 0;
}

/* Whether `name` (`len` bytes) is a name a FAT holds: ASCII, printable, none
 * of "*?<>|":/\, not "." or "..", not ending in a dot or a space. */
static int NameHeld(const uint8_t* name, uint32_t len)
{
    if (len == 0 || len > FAT_NAME_MOST || name[len - 1] == '.' || name[len - 1] == ' ')
        return 0;
    for (uint32_t i = 0; i < len; i++) {
        uint8_t c = name[i];
        if (c < 0x20 || c >= 0x7F || c == '*' || c == '?' || c == '<' || c == '>' || c == '|' || c == '"' ||
            c == ':' || c == '/' || c == '\\')
            return 0;
    }
    return 1;
}

/* `name` as a short name exactly — into `out` (11 bytes) with its case bits
 * in *bits — or 0 when it takes a long name. */
static int ShortExactly(const uint8_t* name, uint32_t len, uint8_t* out, uint8_t* bits)
{
    uint32_t dot = len;
    for (uint32_t i = 0; i < len; i++)
        if (name[i] == '.') {
            if (dot != len)
                return 0;
            dot = i;
        }
    uint32_t base = dot, ext = dot == len ? 0 : len - dot - 1u;
    if (base == 0 || base > 8u || ext > 3u)
        return 0;
    int lower[2] = { 0, 0 }, upper[2] = { 0, 0 };
    Fill(out, ' ', 11);
    for (uint32_t i = 0; i < len; i++) {
        if (i == dot)
            continue;
        uint8_t c = name[i];
        int     part = i > dot;
        if (!ShortChar(c))
            return 0;
        lower[part] |= IsLower(c);
        upper[part] |= IsUpper(c);
        out[part ? 8u + (i - dot - 1u) : i] = Upper(c);
    }
    if ((lower[0] && upper[0]) || (lower[1] && upper[1]))
        return 0;
    *bits = (uint8_t)((lower[0] ? FAT_CASE_BASE : 0) | (lower[1] ? FAT_CASE_EXT : 0));
    return 1;
}

static uint8_t ShortSum(const uint8_t* name11)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1u) << 7) + (sum >> 1) + name11[i]);
    return sum;
}

/* A short entry's name as it is read — "BASE.EXT", its case bits applied —
 * into `out` (13 bytes, ended); its length. */
static uint32_t ShortSpelled(const uint8_t* e, uint8_t* out)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < 8u && e[i] != ' '; i++)
        out[n++] = (e[12] & FAT_CASE_BASE) && IsUpper(e[i]) ? (uint8_t)(e[i] + 32) : e[i];
    if (out[0] == 0x05)
        out[0] = FAT_DELETED;   /* a name that begins with 0xE5 is kept as 0x05 */
    if (e[8] != ' ') {
        out[n++] = '.';
        for (uint32_t i = 8; i < 11u && e[i] != ' '; i++)
            out[n++] = (e[12] & FAT_CASE_EXT) && IsUpper(e[i]) ? (uint8_t)(e[i] + 32) : e[i];
    }
    out[n] = 0;
    return n;
}

static int SameName(const uint8_t* a, uint32_t alen, const uint8_t* b, uint32_t blen)
{
    if (alen != blen)
        return 0;
    for (uint32_t i = 0; i < alen; i++)
        if (Upper(a[i]) != Upper(b[i]))
            return 0;
    return 1;
}

// ----------------------------------------------------------------------------
// Directories
// ----------------------------------------------------------------------------

/* Slot `slot` of directory `dir`: the sector it lies in and its offset there. */
static int SlotAt(FatVolume* v, uint32_t dir, uint32_t slot, uint64_t* sector, uint32_t* offset)
{
    uint32_t per = v->cluster_bytes / FAT_SLOT, c = dir;
    for (uint32_t k = slot / per; k; k--) {
        uint32_t next;
        int      rc = ChainNext(v, c, &next);
        if (rc != FAT_OK)
            return rc;
        if (next >= FAT_EOC)
            return FAT_DAMAGED;
        c = next;
    }
    uint32_t within = (slot % per) * FAT_SLOT;
    *sector = ClusterSector(v, c) + within / v->sector_bytes;
    *offset = within % v->sector_bytes;
    return FAT_OK;
}

static int SlotWrite(FatVolume* v, uint32_t dir, uint32_t slot, const uint8_t* entry)
{
    uint64_t sector;
    uint32_t offset;
    int      rc = SlotAt(v, dir, slot, &sector, &offset);
    if (rc != FAT_OK)
        return rc;
    if (Read(v, sector, 1, v->buf) != FAT_OK)
        return FAT_IO;
    Copy(v->buf + offset, entry, FAT_SLOT);
    return Write(v, sector, 1, v->buf);
}

/* A walk over a directory's slots: what the look at each is told and keeps. */
typedef struct {
    const uint8_t* want;        /* the name looked for, or NULL */
    uint32_t       want_len;
    const uint8_t* alias;       /* a short name looked for (11 bytes), or NULL */
    uint32_t       need;        /* free slots in one run looked for, or 0 */
    uint16_t       lfn[FAT_NAME_MOST + FAT_LONG_CHARS + 1u];
    uint32_t       lfn_left;    /* the long name's slots still to come, 0 for none under way */
    uint32_t       lfn_count;
    uint32_t       lfn_first;
    uint8_t        lfn_sum;
    int            ended;       /* the end mark met: every slot after it free */
    uint32_t       free_run, free_first;
    uint32_t       live;        /* entries met but "." and ".." */
    FatEntry       found;
} DirLook;

/* The look at slot `slot` (`e`): 1 when the walk is to stop, 0 to go on. */
static int SlotLook(FatVolume* v, DirLook* d, uint32_t dir, uint32_t slot, const uint8_t* e)
{
    (void)v;
    int vacant = d->ended || e[0] == 0 || e[0] == FAT_DELETED;
    if (e[0] == 0)
        d->ended = 1;
    if (vacant) {
        d->lfn_left = 0;
        if (d->free_run++ == 0)
            d->free_first = slot;
        return d->need && d->free_run >= d->need;
    }
    d->free_run = 0;
    if (e[11] == FAT_ATTR_LONG_NAME) {
        uint32_t ord = e[0] & 0x1Fu;
        if (e[0] & FAT_LAST_LONG) {
            d->lfn_left  = ord;
            d->lfn_count = ord;
            d->lfn_first = slot;
            d->lfn_sum   = e[13];
            if (ord == 0 || ord * FAT_LONG_CHARS > FAT_NAME_MOST + FAT_LONG_CHARS)
                d->lfn_left = 0;
            else
                d->lfn[ord * FAT_LONG_CHARS] = 0;
        } else if (!d->lfn_left || ord != d->lfn_left || e[13] != d->lfn_sum) {
            d->lfn_left = 0;
            return 0;
        }
        if (!d->lfn_left || ord != d->lfn_left)
            return 0;
        static const uint8_t at[FAT_LONG_CHARS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
        for (uint32_t i = 0; i < FAT_LONG_CHARS; i++)
            d->lfn[(ord - 1u) * FAT_LONG_CHARS + i] = Get16(e + at[i]);
        d->lfn_left--;
        if (d->lfn_left == 0)
            d->lfn_left = ~0u;          /* complete: the short entry is next */
        return 0;
    }
    int long_named = d->lfn_left == ~0u && ShortSum(e) == d->lfn_sum;
    d->lfn_left    = 0;
    if (e[11] & FAT_ATTR_VOLUME_ID)
        return 0;
    if (e[0] == '.' && (e[1] == ' ' || (e[1] == '.' && e[2] == ' ')))
        return 0;
    d->live++;
    int match = 0;
    if (d->alias)
        match = SameName(e, 11, d->alias, 11);
    if (d->want && !match && long_named) {
        uint32_t n = 0;
        while (n < d->lfn_count * FAT_LONG_CHARS && d->lfn[n] && d->lfn[n] != 0xFFFFu)
            n++;
        match = n == d->want_len;
        for (uint32_t i = 0; match && i < n; i++)
            match = d->lfn[i] < 0x80u && Upper((uint8_t)d->lfn[i]) == Upper(d->want[i]);
    }
    if (d->want && !match) {
        uint8_t  spelled[13];
        uint32_t n = ShortSpelled(e, spelled);
        match      = SameName(spelled, n, d->want, d->want_len);
    }
    if (!match)
        return 0;
    d->found = (FatEntry){ .cluster = (uint32_t)Get16(e + 20) << 16 | Get16(e + 26), .size = Get32(e + 28),
                           .attr = e[11], .dir = dir, .first_slot = long_named ? d->lfn_first : slot,
                           .slot = slot };
    return 1;
}

/* Every slot of directory `dir` looked at in order until the look stops the
 * walk (FAT_OK) or the directory ends (FAT_NOT_FOUND); *slots how many there
 * are when it ends. */
static int DirWalk(FatVolume* v, uint32_t dir, DirLook* d, uint32_t* slots)
{
    uint32_t c = dir, slot = 0;
    for (uint32_t guard = 0; guard <= v->clusters; guard++) {
        if (!ClusterValid(v, c))
            return FAT_DAMAGED;
        for (uint32_t s = 0; s < v->cluster_sectors; s++) {
            if (Read(v, ClusterSector(v, c) + s, 1, v->buf) != FAT_OK)
                return FAT_IO;
            for (uint32_t o = 0; o < v->sector_bytes; o += FAT_SLOT, slot++)
                if (SlotLook(v, d, dir, slot, v->buf + o))
                    return FAT_OK;
        }
        uint32_t next;
        int      rc = ChainNext(v, c, &next);
        if (rc != FAT_OK)
            return rc;
        if (next >= FAT_EOC) {
            if (slots)
                *slots = slot;
            return FAT_NOT_FOUND;
        }
        c = next;
    }
    return FAT_DAMAGED;
}

static int DirLookUp(FatVolume* v, uint32_t dir, const uint8_t* name, uint32_t len, FatEntry* out)
{
    DirLook d;
    Fill((uint8_t*)&d, 0, sizeof(d));
    d.want     = name;
    d.want_len = len;
    int rc     = DirWalk(v, dir, &d, NULL);
    if (rc == FAT_OK)
        *out = d.found;
    return rc;
}

/* A short alias for a long `name` no entry of `dir` holds: its basis, six
 * characters, "~N" and the extension. */
static int AliasMade(FatVolume* v, uint32_t dir, const uint8_t* name, uint32_t len, uint8_t* out)
{
    uint32_t dot = len;
    for (uint32_t i = 0; i < len; i++)
        if (name[i] == '.')
            dot = i;
    uint8_t  base[8], ext[3];
    uint32_t nb = 0, ne = 0;
    for (uint32_t i = 0; i < dot && nb < 8u; i++)
        if (name[i] != ' ' && name[i] != '.')
            base[nb++] = ShortChar(name[i]) ? Upper(name[i]) : '_';
    for (uint32_t i = dot + 1u; i < len && ne < 3u; i++)
        if (name[i] != ' ')
            ext[ne++] = ShortChar(name[i]) ? Upper(name[i]) : '_';
    if (nb == 0)
        base[nb++] = '_';
    for (uint32_t n = 1; n < 1000000u; n++) {
        uint8_t  tail[8];
        uint32_t nt = 0;
        for (uint32_t k = n; k; k /= 10u)
            tail[nt++] = (uint8_t)('0' + k % 10u);
        uint32_t keep = nb < 7u - nt ? nb : 7u - nt;
        Fill(out, ' ', 11);
        Copy(out, base, keep);
        out[keep] = '~';
        for (uint32_t i = 0; i < nt; i++)
            out[keep + 1u + i] = tail[nt - 1u - i];
        Copy(out + 8, ext, ne);
        DirLook d;
        Fill((uint8_t*)&d, 0, sizeof(d));
        d.alias = out;
        int rc  = DirWalk(v, dir, &d, NULL);
        if (rc == FAT_NOT_FOUND)
            return FAT_OK;
        if (rc != FAT_OK)
            return rc;
    }
    return FAT_BAD_NAME;
}

/* `need` free slots in one run of directory `dir` into *first — the
 * directory grown by a cluster when it has none. */
static int SlotsFound(FatVolume* v, uint32_t dir, uint32_t need, uint32_t* first)
{
    for (uint32_t round = 0; round < 2u; round++) {
        DirLook d;
        uint32_t slots = 0;
        Fill((uint8_t*)&d, 0, sizeof(d));
        d.need = need;
        int rc = DirWalk(v, dir, &d, &slots);
        if (rc == FAT_OK) {
            *first = d.free_first;
            return FAT_OK;
        }
        if (rc != FAT_NOT_FOUND)
            return rc;
        uint32_t last = dir, next, grown;
        while ((rc = ChainNext(v, last, &next)) == FAT_OK && next < FAT_EOC)
            last = next;
        if (rc != FAT_OK)
            return rc;
        if ((rc = RunTaken(v, (need * FAT_SLOT + v->cluster_bytes - 1u) / v->cluster_bytes, &grown)) != FAT_OK)
            return rc;
        for (uint32_t c = grown; ; c++) {
            if (ClusterZeroed(v, c) != FAT_OK)
                return FAT_IO;
            uint32_t after;
            if (FatGet(v, c, &after) != FAT_OK)
                return FAT_IO;
            if (after >= FAT_EOC)
                break;
        }
        if (FatSet(v, last, grown) != FAT_OK || Commit(v) != FAT_OK)
            return FAT_IO;
    }
    return FAT_NO_ROOM;
}

/* A short entry: its name, attributes, case bits, first cluster and size,
 * stamped. */
static void EntryMade(const FatVolume* v, uint8_t* e, const uint8_t* name11, uint8_t attr, uint8_t bits,
                      uint32_t cluster, uint32_t size)
{
    Fill(e, 0, FAT_SLOT);
    Copy(e, name11, 11);
    e[11] = attr;
    e[12] = bits;
    Put16(e + 14, v->stamp_time);
    Put16(e + 16, v->stamp_date);
    Put16(e + 18, v->stamp_date);
    Put16(e + 20, cluster >> 16);
    Put16(e + 22, v->stamp_time);
    Put16(e + 24, v->stamp_date);
    Put16(e + 26, cluster);
    Put32(e + 28, size);
}

/* An entry named `name` made in directory `dir` — its long name's slots
 * first when it takes one — pointing at `cluster`, `size` long. */
static int EntryAdded(FatVolume* v, uint32_t dir, const uint8_t* name, uint32_t len, uint8_t attr,
                      uint32_t cluster, uint32_t size, FatEntry* out)
{
    uint8_t  short_name[11], bits = 0, e[FAT_SLOT];
    int      exact = ShortExactly(name, len, short_name, &bits);
    uint32_t longs = exact ? 0 : (len + FAT_LONG_CHARS - 1u) / FAT_LONG_CHARS, first;
    int      rc    = exact ? FAT_OK : AliasMade(v, dir, name, len, short_name);
    if (rc == FAT_OK)
        rc = SlotsFound(v, dir, longs + 1u, &first);
    if (rc != FAT_OK)
        return rc;
    static const uint8_t at[FAT_LONG_CHARS] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    uint8_t sum = ShortSum(short_name);
    for (uint32_t k = 0; k < longs; k++) {
        uint32_t ord = longs - k;
        Fill(e, 0, FAT_SLOT);
        e[0]  = (uint8_t)(ord | (k == 0 ? FAT_LAST_LONG : 0u));
        e[11] = FAT_ATTR_LONG_NAME;
        e[13] = sum;
        for (uint32_t i = 0; i < FAT_LONG_CHARS; i++) {
            uint32_t at_char = (ord - 1u) * FAT_LONG_CHARS + i;
            Put16(e + at[i], at_char < len ? name[at_char] : at_char == len ? 0u : 0xFFFFu);
        }
        if ((rc = SlotWrite(v, dir, first + k, e)) != FAT_OK)
            return rc;
    }
    EntryMade(v, e, short_name, attr, bits, cluster, size);
    if ((rc = SlotWrite(v, dir, first + longs, e)) != FAT_OK)
        return rc;
    if (out)
        *out = (FatEntry){ .cluster = cluster, .size = size, .attr = attr, .dir = dir, .first_slot = first,
                           .slot = first + longs };
    return FAT_OK;
}

/* The next part of `path` from *at into *part and *len; 0 when none is left. */
static int PathPart(const char** at, const uint8_t** part, uint32_t* len)
{
    const char* p = *at;
    while (*p == '/' || *p == '\\')
        p++;
    if (!*p)
        return 0;
    const char* end = p;
    while (*end && *end != '/' && *end != '\\')
        end++;
    *part = (const uint8_t*)p;
    *len  = (uint32_t)(end - p);
    *at   = end;
    return 1;
}

/* The directory `path` names — every part a directory — into *dir. The root
 * for an empty path; with `make`, every part not yet there made. */
static int DirOf(FatVolume* v, const char* path, uint32_t path_len, int make, FatEntry* last, uint32_t* dir)
{
    const char*    at = path;
    const uint8_t* part;
    uint32_t       len;
    *dir = v->root;
    while ((uint32_t)(at - path) < path_len && PathPart(&at, &part, &len)) {
        if ((uint32_t)(at - path) > path_len)
            break;
        if (!NameHeld(part, len))
            return FAT_BAD_NAME;
        FatEntry e;
        int      rc = DirLookUp(v, *dir, part, len, &e);
        if (rc == FAT_NOT_FOUND && make) {
            uint32_t c;
            uint8_t  dots[FAT_SLOT], name[11];
            if ((rc = RunTaken(v, 1, &c)) != FAT_OK || (rc = ClusterZeroed(v, c)) != FAT_OK)
                return rc;
            Fill(name, ' ', 11);
            name[0] = '.';
            EntryMade(v, dots, name, FAT_ATTR_DIRECTORY, 0, c, 0);
            if ((rc = SlotWrite(v, c, 0, dots)) != FAT_OK)
                return rc;
            name[1] = '.';
            EntryMade(v, dots, name, FAT_ATTR_DIRECTORY, 0, *dir == v->root ? 0 : *dir, 0);
            if ((rc = SlotWrite(v, c, 1, dots)) != FAT_OK || (rc = Commit(v)) != FAT_OK)
                return rc;
            if ((rc = EntryAdded(v, *dir, part, len, FAT_ATTR_DIRECTORY, c, 0, &e)) != FAT_OK ||
                (rc = Commit(v)) != FAT_OK)
                return rc;
        } else if (rc != FAT_OK) {
            return rc;
        }
        if (!(e.attr & FAT_ATTR_DIRECTORY))
            return FAT_NOT_DIRECTORY;
        *dir = e.cluster ? e.cluster : v->root;
        if (last)
            *last = e;
    }
    return FAT_OK;
}

/* `path` parted into its directory's part (`dir_len` bytes) and its last
 * name. FAT_BAD_NAME for no last name. */
static int PathSplit(const char* path, uint32_t* dir_len, const uint8_t** name, uint32_t* len)
{
    const char*    at = path;
    const uint8_t* part = NULL;
    uint32_t       n = 0;
    while (PathPart(&at, &part, &n)) {
        *dir_len = (uint32_t)((const char*)part - path);
        *name    = part;
        *len     = n;
    }
    return part && NameHeld(*name, *len) ? FAT_OK : FAT_BAD_NAME;
}

// ----------------------------------------------------------------------------
// The calls
// ----------------------------------------------------------------------------

/* Seconds since 1970 as a DOS date and time — 1980 to 2107, held to them. */
void FatStamp(FatVolume* v, uint64_t unix_seconds)
{
    uint64_t days = unix_seconds / 86400u, secs = unix_seconds % 86400u;
    uint64_t z = days + 719468u, era = z / 146097u, doe = z - era * 146097u;
    uint64_t yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    uint64_t doy = doe - (365u * yoe + yoe / 4u - yoe / 100u), mp = (5u * doy + 2u) / 153u;
    uint64_t day = doy - (153u * mp + 2u) / 5u + 1u, month = mp < 10u ? mp + 3u : mp - 9u;
    uint64_t year = yoe + era * 400u + (month <= 2u);
    if (year < 1980u) {
        year = 1980u, month = 1u, day = 1u, secs = 0;
    } else if (year > 2107u) {
        year = 2107u, month = 12u, day = 31u, secs = 86399u;
    }
    v->stamp_date = (uint16_t)((year - 1980u) << 9 | month << 5 | day);
    v->stamp_time = (uint16_t)((secs / 3600u) << 11 | (secs / 60u % 60u) << 5 | (secs % 60u) / 2u);
}

int FatOpen(FatVolume* v, const FatMedium* m)
{
    uint8_t* b = v->buf;
    v->medium  = m;
    v->fat_at  = ~(uint64_t)0;
    v->fat_dirty = 0;
    if (m->sector_bytes < 512u || m->sector_bytes > FAT_SECTOR_MOST || m->sector_bytes % 512u)
        return FAT_NOT_FAT32;
    v->sector_bytes = m->sector_bytes;
    if (Read(v, 0, 1, b) != FAT_OK)
        return FAT_IO;
    uint32_t spc = b[13], total = Get32(b + 32), fat_sectors = Get32(b + 36);
    if (b[510] != 0x55 || b[511] != 0xAA || Get16(b + 11) != m->sector_bytes || spc == 0 || (spc & (spc - 1u)) ||
        Get16(b + 14) == 0 || b[16] == 0 || Get16(b + 17) != 0 || Get16(b + 19) != 0 || Get16(b + 22) != 0 ||
        fat_sectors == 0 || total > m->sectors)
        return FAT_NOT_FAT32;
    v->cluster_sectors = spc;
    v->cluster_bytes   = spc * m->sector_bytes;
    v->reserved        = Get16(b + 14);
    v->fats            = b[16];
    v->fat_sectors     = fat_sectors;
    v->data_start      = v->reserved + (uint64_t)v->fats * fat_sectors;
    if (v->data_start >= total)
        return FAT_NOT_FAT32;
    uint64_t clusters = (total - v->data_start) / spc;
    if (clusters < FAT_CLUSTERS_LEAST || clusters > FAT_CLUSTERS_MOST ||
        (clusters + 2u) * 4u > (uint64_t)fat_sectors * m->sector_bytes)
        return FAT_NOT_FAT32;
    v->clusters   = (uint32_t)clusters;
    v->root       = Get32(b + 44);
    uint32_t ext  = Get16(b + 40), info = Get16(b + 48);
    if (!ClusterValid(v, v->root) || (ext & 0x80u))
        return FAT_NOT_FAT32;     /* one FAT active alone: a layout this code does not write */
    v->fsinfo     = 0;
    v->free_count = -1;
    v->free_hint  = 2u;
    if (info && info < v->reserved && Read(v, info, 1, b) == FAT_OK && Get32(b) == 0x41615252u &&
        Get32(b + 484) == 0x61417272u) {
        uint32_t count = Get32(b + 488), hint = Get32(b + 492);
        v->fsinfo      = info;
        v->free_count  = count <= v->clusters ? (int64_t)count : -1;
        v->free_hint   = ClusterValid(v, hint) ? hint : 2u;
    }
    FatStamp(v, 0);
    return FAT_OK;
}

int FatLay(const FatMedium* m, FatVolume* work, uint64_t hidden, const char* label, uint32_t volume_id)
{
    uint32_t s = m->sector_bytes;
    if (s < 512u || s > FAT_SECTOR_MOST || s % 512u)
        return FAT_TOO_SMALL;
    /* Microsoft's table, in bytes: a cluster of 512 to 260 MB, 4K to 8 GB,
     * 8K to 16 GB, 16K to 32 GB, 32K past — never less than a sector. */
    uint64_t bytes   = m->sectors * s;
    uint32_t cluster = bytes <= (260ull << 20) ? 512u : bytes <= (8ull << 30) ? 4096u
                     : bytes <= (16ull << 30) ? 8192u : bytes <= (32ull << 30) ? 16384u : 32768u;
    uint32_t spc     = cluster < s ? 1u : cluster / s;
    /* The data region begins on a cluster and on 4 KiB, the physical block
     * of every disk of 512-byte sectors made since 2011 (UEFI 2.10 §13.3.1.1
     * asks it aligned to the physical block): the reserved sectors grow to
     * put it there. */
    uint32_t align   = spc > 4096u / s ? spc : (4096u / s ? 4096u / s : 1u);
    uint64_t fat     = 1, clusters = 0, reserved = FAT_RESERVED;
    for (uint32_t round = 0; round < 64u; round++) {
        if (m->sectors <= reserved + 2u * fat)
            return FAT_TOO_SMALL;
        clusters      = (m->sectors - reserved - 2u * fat) / spc;
        uint64_t need = ((clusters + 2u) * 4u + s - 1u) / s;
        uint64_t pad  = (align - (FAT_RESERVED + 2u * need) % align) % align;
        if (need == fat && reserved == FAT_RESERVED + pad)
            break;
        fat      = need;
        reserved = FAT_RESERVED + pad;
    }
    if (clusters < FAT_CLUSTERS_LEAST)
        return FAT_TOO_SMALL;
    if (clusters > FAT_CLUSTERS_MOST)
        clusters = FAT_CLUSTERS_MOST;
    uint64_t total = reserved + 2u * fat + clusters * spc;

    FatVolume* v = work;
    v->medium    = m;
    v->sector_bytes = s;
    uint8_t* b   = v->buf;
    /* The reserved sectors and both FATs zeroed, as many sectors a write as
     * the buffer holds. */
    uint32_t per = FAT_SECTOR_MOST / s;
    Fill(b, 0, FAT_SECTOR_MOST);
    for (uint64_t at = 0, end = reserved + 2u * fat; at < end; at += per)
        if (Write(v, at, end - at < per ? (uint32_t)(end - at) : per, b) != FAT_OK)
            return FAT_IO;

    b[0] = 0xEB, b[1] = 0x58, b[2] = 0x90;
    Copy(b + 3, (const uint8_t*)"BOXOS   ", 8);
    Put16(b + 11, s);
    b[13] = (uint8_t)spc;
    Put16(b + 14, (uint32_t)reserved);
    b[16] = 2;
    b[21] = 0xF8;
    Put16(b + 24, 63);
    Put16(b + 26, 255);
    Put32(b + 28, hidden > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)hidden);
    Put32(b + 32, (uint32_t)total);
    Put32(b + 36, (uint32_t)fat);
    Put32(b + 44, 2);           /* the root's cluster */
    Put16(b + 48, 1);           /* FSInfo */
    Put16(b + 50, 6);           /* the boot sector's copy */
    b[64] = 0x80;
    b[66] = 0x29;
    Put32(b + 67, volume_id);
    Fill(b + 71, ' ', 11);
    for (uint32_t i = 0; label && label[i] && i < 11u; i++)
        b[71 + i] = Upper((uint8_t)label[i]);
    if (!label)
        Copy(b + 71, (const uint8_t*)"NO NAME    ", 11);
    Copy(b + 82, (const uint8_t*)"FAT32   ", 8);
    b[510] = 0x55, b[511] = 0xAA;
    if (Write(v, 0, 1, b) != FAT_OK || Write(v, 6, 1, b) != FAT_OK)
        return FAT_IO;

    Fill(b, 0, s);
    Put32(b, 0x41615252u);
    Put32(b + 484, 0x61417272u);
    Put32(b + 488, (uint32_t)clusters - 1u);    /* the root's cluster is taken */
    Put32(b + 492, 3);
    Put32(b + 508, 0xAA550000u);
    if (Write(v, 1, 1, b) != FAT_OK || Write(v, 7, 1, b) != FAT_OK)
        return FAT_IO;

    Fill(b, 0, s);
    Put32(b, 0x0FFFFFF8u);
    Put32(b + 4, 0x0FFFFFFFu);
    Put32(b + 8, 0x0FFFFFFFu);  /* the root: one cluster, ended */
    if (Write(v, reserved, 1, b) != FAT_OK || Write(v, reserved + fat, 1, b) != FAT_OK)
        return FAT_IO;
    /* The root, empty but for the label's entry: the place every system
     * reads a volume's label from — the boot sector's copy is the old one. */
    Fill(b, 0, s);
    if (label) {
        Fill(b, ' ', 11);
        for (uint32_t i = 0; label[i] && i < 11u; i++)
            b[i] = Upper((uint8_t)label[i]);
        b[11] = FAT_ATTR_VOLUME_ID;
    }
    for (uint32_t i = 0; i < spc; i++) {
        if (Write(v, reserved + 2u * fat + i, 1, b) != FAT_OK)
            return FAT_IO;
        Fill(b, 0, FAT_SLOT);
    }
    return m->flush(m->ctx) == 0 ? FAT_OK : FAT_IO;
}

int FatFind(FatVolume* v, const char* path, FatEntry* out)
{
    uint32_t       dir_len = 0, len = 0, dir;
    const uint8_t* name;
    const char*    at = path;
    const uint8_t* part;
    uint32_t       n;
    if (!PathPart(&at, &part, &n)) {
        *out = (FatEntry){ .cluster = v->root, .attr = FAT_ATTR_DIRECTORY, .dir = v->root };
        return FAT_OK;
    }
    int rc = PathSplit(path, &dir_len, &name, &len);
    if (rc == FAT_OK)
        rc = DirOf(v, path, dir_len, 0, NULL, &dir);
    return rc == FAT_OK ? DirLookUp(v, dir, name, len, out) : rc;
}

int FatMakeDirs(FatVolume* v, const char* path, FatEntry* out)
{
    uint32_t dir, len = 0;
    while (path[len])
        len++;
    FatEntry last = { .cluster = v->root, .attr = FAT_ATTR_DIRECTORY, .dir = v->root };
    int      rc   = DirOf(v, path, len, 1, &last, &dir);
    if (rc == FAT_OK && out)
        *out = last;
    return rc;
}

int FatPut(FatVolume* v, const char* path, const void* data, uint32_t bytes, FatEntry* out)
{
    uint32_t       dir_len = 0, len = 0, dir, first = 0;
    const uint8_t* name;
    FatEntry       was;
    int            rc = PathSplit(path, &dir_len, &name, &len);
    if (rc == FAT_OK)
        rc = DirOf(v, path, dir_len, 1, NULL, &dir);
    if (rc != FAT_OK)
        return rc;
    rc = DirLookUp(v, dir, name, len, &was);
    if (rc != FAT_OK && rc != FAT_NOT_FOUND)
        return rc;
    int there = rc == FAT_OK;
    if (there && (was.attr & FAT_ATTR_DIRECTORY))
        return FAT_NOT_DIRECTORY;

    /* The bytes into clusters no entry names, and on the medium with the
     * FATs that chain them. */
    uint32_t n = (bytes + v->cluster_bytes - 1u) / v->cluster_bytes;
    if (n && (rc = RunTaken(v, n, &first)) != FAT_OK)
        return rc;
    uint64_t       sector = n ? ClusterSector(v, first) : 0, span = (uint64_t)n * v->cluster_sectors;
    uint32_t       whole  = data ? bytes / v->sector_bytes : 0, done = 0;
    const uint8_t* from   = data;
    if (whole && Write(v, sector, whole, from) != FAT_OK)
        return FAT_IO;
    for (done = whole; done < span; done++) {
        uint32_t off = done * v->sector_bytes;
        Fill(v->buf, 0, v->sector_bytes);
        if (data && off < bytes)
            Copy(v->buf, from + off, bytes - off < v->sector_bytes ? bytes - off : v->sector_bytes);
        if (Write(v, sector + done, 1, v->buf) != FAT_OK)
            return FAT_IO;
    }
    if (Commit(v) != FAT_OK)
        return FAT_IO;

    /* Then the entry pointed at them, on the medium; the clusters it held
     * before given back last. */
    FatEntry made;
    if (there) {
        uint64_t es;
        uint32_t eo;
        if ((rc = SlotAt(v, dir, was.slot, &es, &eo)) != FAT_OK || Read(v, es, 1, v->buf) != FAT_OK)
            return rc != FAT_OK ? rc : FAT_IO;
        uint8_t* e = v->buf + eo;
        e[11] |= FAT_ATTR_ARCHIVE;
        Put16(e + 18, v->stamp_date);
        Put16(e + 20, first >> 16);
        Put16(e + 22, v->stamp_time);
        Put16(e + 24, v->stamp_date);
        Put16(e + 26, first);
        Put32(e + 28, bytes);
        if (Write(v, es, 1, v->buf) != FAT_OK || v->medium->flush(v->medium->ctx) != 0)
            return FAT_IO;
        made = was;
        made.cluster = first;
        made.size    = bytes;
        if (ClusterValid(v, was.cluster) && (rc = ChainFreed(v, was.cluster)) != FAT_OK)
            return rc;
    } else if ((rc = EntryAdded(v, dir, name, len, FAT_ATTR_ARCHIVE, first, bytes, &made)) != FAT_OK) {
        return rc;
    }
    if (Commit(v) != FAT_OK)
        return FAT_IO;
    if (out)
        *out = made;
    return FAT_OK;
}

int64_t FatRead(FatVolume* v, const FatEntry* e, uint32_t offset, void* into, uint32_t bytes)
{
    if (offset >= e->size || !bytes)
        return 0;
    if (bytes > e->size - offset)
        bytes = e->size - offset;
    uint8_t* to = into;
    uint32_t c = e->cluster, got = 0;
    for (uint32_t skip = offset / v->cluster_bytes; skip; skip--) {
        uint32_t next;
        int      rc = ChainNext(v, c, &next);
        if (rc != FAT_OK || next >= FAT_EOC)
            return rc != FAT_OK ? rc : FAT_DAMAGED;
        c = next;
    }
    uint32_t at = offset % v->cluster_bytes;
    while (got < bytes) {
        if (!ClusterValid(v, c))
            return FAT_DAMAGED;
        uint32_t sector = at / v->sector_bytes, within = at % v->sector_bytes;
        if (Read(v, ClusterSector(v, c) + sector, 1, v->buf) != FAT_OK)
            return FAT_IO;
        uint32_t take = v->sector_bytes - within < bytes - got ? v->sector_bytes - within : bytes - got;
        Copy(to + got, v->buf + within, take);
        got += take;
        at  += take;
        if (at == v->cluster_bytes && got < bytes) {
            uint32_t next;
            int      rc = ChainNext(v, c, &next);
            if (rc != FAT_OK || next >= FAT_EOC)
                return rc != FAT_OK ? rc : FAT_DAMAGED;
            c  = next;
            at = 0;
        }
    }
    return got;
}

int FatRun(FatVolume* v, const FatEntry* e, uint64_t* first, uint64_t* count)
{
    if (!ClusterValid(v, e->cluster))
        return e->cluster ? FAT_DAMAGED : FAT_NOT_FOUND;
    uint32_t c = e->cluster, n = 1;
    for (;; n++) {
        uint32_t next;
        int      rc = ChainNext(v, c, &next);
        if (rc != FAT_OK)
            return rc;
        if (next >= FAT_EOC)
            break;
        if (next != c + 1u || n > v->clusters)
            return FAT_DAMAGED;
        c = next;
    }
    if ((uint64_t)n * v->cluster_bytes < e->size)
        return FAT_DAMAGED;
    *first = ClusterSector(v, e->cluster);
    *count = (uint64_t)n * v->cluster_sectors;
    return FAT_OK;
}

int FatRemove(FatVolume* v, const char* path)
{
    FatEntry e;
    int      rc = FatFind(v, path, &e);
    if (rc != FAT_OK)
        return rc;
    if (e.slot == e.first_slot && e.dir == e.cluster)
        return FAT_BAD_NAME;    /* the root */
    if (e.attr & FAT_ATTR_DIRECTORY) {
        DirLook d;
        Fill((uint8_t*)&d, 0, sizeof(d));
        rc = DirWalk(v, e.cluster, &d, NULL);
        if (rc != FAT_NOT_FOUND)
            return rc == FAT_OK ? FAT_NOT_DIRECTORY : rc;
        if (d.live)
            return FAT_NOT_DIRECTORY;
    }
    uint8_t gone[FAT_SLOT];
    for (uint32_t s = e.first_slot; s <= e.slot; s++) {
        uint64_t sector;
        uint32_t offset;
        if ((rc = SlotAt(v, e.dir, s, &sector, &offset)) != FAT_OK || Read(v, sector, 1, v->buf) != FAT_OK)
            return rc != FAT_OK ? rc : FAT_IO;
        Copy(gone, v->buf + offset, FAT_SLOT);
        gone[0] = FAT_DELETED;
        Copy(v->buf + offset, gone, FAT_SLOT);
        if (Write(v, sector, 1, v->buf) != FAT_OK)
            return FAT_IO;
    }
    if (v->medium->flush(v->medium->ctx) != 0)
        return FAT_IO;
    if (ClusterValid(v, e.cluster) && (rc = ChainFreed(v, e.cluster)) != FAT_OK)
        return rc;
    return Commit(v);
}
