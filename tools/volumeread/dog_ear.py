"""dog_ear — the keys of the book's trees the stitches past a slot's `at`
changed, and what a Tally laid in after them is reckoned to cost (the
kernel's settle/dog_ear.c and settle/binder_cost.c, one rule with both).

A stitch changes, of FILES, every file it carries a record of: the header of
one changed, and every Parcel of either side the other does not hold at the
same block — closed at the next Parcel both hold there, or at the file's end;
the whole id of one born or given up. Of DIGESTS, every window of
DIGEST_ITEM_MOST blocks whose ground one side names and the other does not. Of
TAGS, the ids it coins; of USE, its one key, when it carries a Use Context.
The kernel tells a Parcel the two records share by its memory; this reader,
by what it holds and where.

A Tally laid in lays again every leaf a changed key lies in — the key's leaf
is the last whose lower key is not past it, the first for a key below every
separator — and every inner node over one of them; the next Tally is reckoned
at the Sheets of those, both copies, and the slot, never more than the whole
book's. A leaf at what is left live in it: its Sheets while a key of the state
lies in its range, none when none does — its path all the same, the parent
that drops it laid (§16.6 п.6; binder.py Lives says which do). A tree the
Tally before has no node of costs a block once a key of it changed.
"""

import bisect

from .binder import DIGEST_ITEM_MOST, Parcels
from .tally import DIGESTS, FILES, TAGS, USE


def Fresh():
    """Spans of keys [lo, hi) by tree, hi None for a span to the tree's end."""
    return {FILES: [], TAGS: [], USE: [], DIGESTS: []}


def _Windows(spans, start, end):
    lo = start // DIGEST_ITEM_MOST * DIGEST_ITEM_MOST
    spans[DIGESTS].append((lo, ((end - 1) // DIGEST_ITEM_MOST + 1) * DIGEST_ITEM_MOST))


def _Joined(runs):
    out = []
    for s, c in sorted(runs):
        if out and s <= out[-1][1]:
            out[-1][1] = max(out[-1][1], s + c)
        else:
            out.append([s, s + c])
    return out


def _Differed(spans, was, now):
    """Every window of ground one side names and the other does not."""
    points, held, x = [], [0, 0], None
    for side, runs in enumerate((was, now)):
        for s, e in _Joined(runs):
            points += [(s, side, 1), (e, side, -1)]
    for p, side, d in sorted(points):
        if x is not None and p > x and (held[0] > 0) != (held[1] > 0):
            _Windows(spans, x, p)
        held[side] += d
        x = p


def _Placed(parcels):
    out, at = [], 0
    for runs in parcels:
        out.append((at, runs))
        at += sum(r.count for r in runs)
    return out


def _Ground(runs):
    return [(r.start, r.count) for r in runs if not r.hole]


def FileChanged(spans, fid, was, now):
    """File `fid` from `was` to `now` (None for none) — dog_ear.c
    DogEarFileChanged."""
    end = (fid + 1, 0)
    if was is None and now is None:
        return
    if was is None or now is None:
        spans[FILES].append(((fid, 0), end))
        _Differed(spans, was.Ground() if was else [], now.Ground() if now else [])
        return
    spans[FILES].append(((fid, 0), (fid, 1)))
    a, b, ground, i, j, opened = _Placed(Parcels(was)), _Placed(Parcels(now)), ([], []), 0, 0, None
    while i < len(a) or j < len(b):
        if i < len(a) and j < len(b) and a[i][0] == b[j][0] and a[i][1] == b[j][1]:
            if opened is not None:
                spans[FILES].append((opened, (fid, 1 + a[i][0])))
            opened = None
            i, j = i + 1, j + 1
            continue
        pos = a[i][0] if j == len(b) or (i < len(a) and a[i][0] <= b[j][0]) else b[j][0]
        if opened is None:
            opened = (fid, 1 + pos)
        if i < len(a) and a[i][0] == pos:
            ground[0].extend(_Ground(a[i][1]))
            i += 1
        if j < len(b) and b[j][0] == pos:
            ground[1].extend(_Ground(b[j][1]))
            j += 1
    if opened is not None:
        spans[FILES].append((opened, end))
    _Differed(spans, ground[0], ground[1])


def Stitched(spans, before, after, fids, coined, used):
    """One stitch applied: the files it carried a record of, from where it
    found them to where it left them; the tag ids it coined; its Use
    Context."""
    for fid in sorted(fids):
        FileChanged(spans, fid, before.get(fid), after.get(fid))
    spans[TAGS].extend((tid, tid + 1) for tid in coined)
    if used:
        spans[USE].append((0, 1))


def _Index(los, key):
    """The node of a run whose lower keys past the first are `los` a key lies
    in."""
    return bisect.bisect_right(los, key)


def _TreeBlocks(tree, nodes, spans, lives):
    leaves = sorted((n for n in nodes if n.level == 0), key=lambda n: (n.lo is not None, n.lo))
    if not leaves:
        return 1
    los = [n.lo for n in leaves[1:]]
    dirty = set()
    for lo, hi in spans:
        i = _Index(los, lo)
        dirty.add(i)
        for k in range(i + 1, len(leaves)):
            if hi is not None and not leaves[k].lo < hi:
                break
            dirty.add(k)
    blocks = sum(leaves[i].sheets for i in dirty
                 if lives(tree, leaves[i].lo if i else None, leaves[i + 1].lo if i + 1 < len(leaves) else None))
    for level in range(1, 1 + max(n.level for n in nodes)):
        run = sorted((n for n in nodes if n.level == level), key=lambda n: (n.lo is not None, n.lo))
        rlos = [n.lo for n in run[1:]]
        over = {0 if i == 0 else _Index(rlos, leaves[i].lo) for i in dirty}
        blocks += sum(run[x].sheets for x in over)
    return blocks


def TipInBlocks(t, spans, lives):
    """The Sheets of one copy of the leaves the spans meet and their path in
    Tally `t` — binder_cost.c BinderTipInBlocks: a leaf only where
    lives(tree, lo, hi) says a key of the state lies in [lo, hi), lo None
    from the tree's first key, hi None to its end."""
    return sum(_TreeBlocks(tree, t.trees[tree].nodes if tree in t.trees else [], ss, lives)
               for tree, ss in spans.items() if ss)
