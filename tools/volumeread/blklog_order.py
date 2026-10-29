"""blklog_order — the order the disk finished the writes of a boot that wrote a
book, from the record blklogwrites kept of it, held to §3.3 by each stitch's
own word: one that says DATA_FLUSHED — its toll paid — data → FLUSH#1 → the
stitch's atoms → FLUSH; one that does not, data → the stitch's atoms → FLUSH,
both behind the one flush.

One rule for every harness that holds a record to it: logcheck's book_circle
(tools/logcheck_book_circle.sh) and PowerCut's judge of a book's run
(tools/powercut_judge_book.py Order). Every stitch the ribbon of the image the
boot left carries is read by volume-read's own reader (bookmount.py); the
boot's are those whose sectors the record wrote. The last write of every block
of fresh data their Born and Shaped records name is made before the stitch's
first atom; of a stitch that says DATA_FLUSHED it stands behind a flush the
atom does not; and the last atom of a stitch stands behind a flush the next
stitch's first does not — which, of a stitch that does not say it, puts its
data behind that flush too, as nothing else would. And a CLEAN stitch — a
stopping machine's last — never spares the flush of its data.
"""

import bisect

from . import blklog, stitch, tally
from .bookground import BookGround
from .bookmount import BookMount
from .medium import Medium
from .ondisk import SECTORS_PER_BLOCK


def Fresh(u):
    """The blocks of fresh data a stitch names: its Born and Shaped records'
    DATA runs but SHARED ones (§2.7, stitch.Fresh)."""
    out = []
    for kind, _ign, body, _at in u.records:
        if kind not in (stitch.BORN, stitch.SHAPED):
            continue
        d = stitch.RecordDecode(kind, body, u.seq)
        runs = d["runs"] if kind == stitch.BORN else [x for _a, _m, rs in d["splices"] for x in rs]
        for x in runs:
            if not x.hole and tally.EXT_SHARED not in stitch.Extensions(x.ext):
                out.extend(range(x.start, x.start + x.count))
    return out


def Order(img, record, ended=True):
    """[(held, what)]: each clause of the rule, as the record of the boot that
    left IMG says it was kept. `ended`: the record ends after the boot's last
    flush — a halt's, which logcheck's book_circle waits for — so the boot
    sewed a stitch of data and its last stitch stands behind a flush; a record
    a power cut ended (PowerCut's) owes neither, and is held to the order
    alone."""
    out = []

    def say(held, text):
        out.append((held, text))

    medium = Medium(img)
    log = blklog.BlkLog(record, medium.SizeSectors())
    writes = [w for w in log.Writes() if not w.discard]
    touched = {}
    for i, w in enumerate(writes):
        for s in range(w.lba, w.lba + w.sectors):
            touched.setdefault(s, []).append(i)
    g = BookGround(medium)
    m = BookMount(g)
    if m.refusal or m.ribbon is None:
        say(False, "the book the boot left does not mount: %s" % (m.refusal or "no ribbon"))
        return out
    sewn = older = with_data = blocks = 0
    torn, spared, unwritten, unflushed, crossed, clean = [], [], [], [], [], []
    before = None
    for u in m.ribbon.stitches:
        at = [touched[g.base + p.Vlba()][-1] for p in u.places if g.base + p.Vlba() in touched]
        if not at:
            older += 1
            before = None
            continue
        if not u.Sound() or len(at) != len(u.places):
            torn.append(u.seq)
            continue
        sewn += 1
        first, last = min(at), max(at)
        paid = bool(u.head.flags & stitch.DATA_FLUSHED)
        if not paid:
            spared.append(u.seq)
            if u.head.flags & stitch.CLEAN:
                clean.append(u.seq)
        if before is not None and writes[before[1]].epoch >= writes[first].epoch:
            crossed.append((before[0], u.seq))
        before = (u.seq, last)
        fresh = Fresh(u)
        with_data += bool(fresh)
        blocks += len(fresh)
        for b in fresh:
            lasts = []
            for k in range(SECTORS_PER_BLOCK):
                seen = touched.get(g.base + b * SECTORS_PER_BLOCK + k, [])
                j = bisect.bisect_left(seen, first)
                lasts.append(seen[j - 1] if j else None)
            if None in lasts:
                unwritten.append((u.seq, b))
            elif paid and writes[max(lasts)].epoch >= writes[first].epoch:
                unflushed.append((u.seq, b))
    tail = before is not None and writes[before[1]].epoch < len(log.epochs) - 1
    say((sewn > 0 and with_data > 0 or not ended) and not torn,
        "the record holds every sector of %u stitch(es) the boot sewed, %u naming %u block(s) of fresh data "
        "(%u laid before it; %s not whole) over %u write(s) and %u flush(es)"
        % (sewn, with_data, blocks, older, torn or "none", len(writes), len(log.flush_seqs)))
    say(not unwritten, "every block of fresh data a stitch names was written before the stitch's first atom "
        "(%u not: %s)" % (len(unwritten), unwritten[:4]))
    say(not unflushed, "and, of a stitch that says DATA_FLUSHED, stands behind a flush that atom does not — data, "
        "then FLUSH#1, then the stitch (%u block(s) not: %s); %u stitch(es) spared it, their data before their "
        "atoms: %s" % (len(unflushed), unflushed[:4], len(spared), spared[:4]))
    say(not clean, "no CLEAN stitch spared the flush of its data (%u did: %s)" % (len(clean), clean[:4]))
    say(not crossed and (tail or not ended), "the atoms of every stitch stand behind a flush the next stitch's first does not, "
        "and the last stitch's behind one (%u crossed: %s; the last behind a flush: %s)" % (len(crossed), crossed[:4], tail))
    return out
