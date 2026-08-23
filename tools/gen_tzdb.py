#!/usr/bin/env python3
"""gen_tzdb.py — bake the IANA time zone database into <__bits/tzdb_table>.

NOT a build dependency. The build compiles the baked constexpr array; run this
by hand when a new IANA release lands, and commit what it writes.

    ./gen_tzdb.py --tzdir <dir produced by `zic -b slim`> \
                  --leap  <leap-seconds.list> \
                  --links <tzdata source files...> \
                  --version 2026c --out include/std/__bits/tzdb_table

What is baked and why this shape
────────────────────────────────
`zic` is the reference compiler of the zone rules, and it runs HERE, on the
host, at generation time. The guest walks a table; it never evaluates a Rule
line. That is the same division the Unicode tables draw, and for the same
reason: the hard part has a reference implementation, so run it where a
reference implementation can be run.

Facts that were MEASURED before this format was chosen, each of which killed a
smaller encoding that looked obviously right:

  * No zone has more than 11 local-time types, and the whole database uses 7
    distinct `save` values. Both indices fit in one nibble, so a transition
    costs ONE byte beyond its timestamp.
  * `zic -b slim` emits 16 631 transitions where `-b fat` emits 23 020. The
    6 389 it leaves out are the future ones it would have expanded to 2037, and
    the POSIX footer reproduces them exactly -- and keeps reproducing them past
    2037, which the fat table stops doing. Slim is both smaller and correct for
    longer, so slim it is. Every one of the 341 zone bodies has a footer, so the
    reader needs no special case for a zone without one.
  * `save` is stored per transition rather than per type. The nibble carrying
    it is free either way, and the derivation it came from went through three
    versions before it was right (see <zonelines.py>): the arithmetic one was
    wrong on 17 927 instants, CPython's per-type one on 733, and the exact one
    reads the STDOFF of the Zone record in force. Storage that can express what
    the last of those computes cost nothing, so it stays able to.

Verification, which is why the compression carries no residual risk
───────────────────────────────────────────────────────────────────
This script decodes its own output and compares it against the TZif input for
every zone, every local-time type and every transition, and against Python's
`zoneinfo` -- an independent reader of the same data -- around every transition
and across a sweep of the baked range. It refuses to write on any disagreement,
and on two invariants besides: a transition is daylight saving exactly when its
save is non-zero, and two zones that compile to the same TZif must agree about
save. Both have fired on real data.

What it does NOT check, and says so in its own output, is time past the last
baked transition: that answer comes from the POSIX footer, which is copied
verbatim here and evaluated in the guest. A second evaluator written beside
this one would agree with the first for the same wrong reason. The footer path
is measured in the host stand instead, against libstdc++ and against `zdump` --
tzcode's own dumper, reading the files tzcode's own compiler produced.
"""

import argparse
import os
import re
import struct
import sys
from pathlib import Path

import zonelines

MAGIC = b"BOXTZDB1"
NO_LINK = 0xFFFF


# ── reading what zic produced ───────────────────────────────────────────────

class Zone:
    __slots__ = ("path", "trans", "types", "abbrev_of", "posix", "fat", "recs")


def parse_tzif(raw, path):
    if raw[:4] != b"TZif":
        return None
    if raw[4:5] == b"\0":
        raise SystemExit(f"{path}: TZif v1; regenerate with `zic -b slim`")

    def counts(off):
        return struct.unpack(">6I", raw[off + 20:off + 44])

    isutc, isstd, leap, tcnt, ttcnt, chars = counts(0)
    v2 = 44 + tcnt * 4 + tcnt + ttcnt * 6 + chars + leap * 8 + isstd + isutc
    isutc, isstd, leap, tcnt, ttcnt, chars = counts(v2)

    p = v2 + 44
    times = list(struct.unpack(f">{tcnt}q", raw[p:p + tcnt * 8])); p += tcnt * 8
    idxs = list(raw[p:p + tcnt]); p += tcnt
    types = []
    for _ in range(ttcnt):
        utoff, isdst, ab = struct.unpack(">iBB", raw[p:p + 6]); p += 6
        types.append((utoff, bool(isdst), ab))
    blob = raw[p:p + chars]; p += chars
    p += leap * 12 + isstd + isutc
    footer = raw[p:].strip(b"\n")

    z = Zone()
    z.fat = None
    z.recs = None
    z.path = path
    z.trans = list(zip(times, idxs))
    z.types = types
    z.abbrev_of = {ab: blob[ab:blob.index(b"\0", ab)].decode()
                   for (_o, _d, ab) in types}
    z.posix = footer.decode()
    if not z.posix:
        raise SystemExit(f"{path}: no POSIX footer — the reader has no rule "
                         f"for time past the last transition")
    if any(times[i] >= times[i + 1] for i in range(len(times) - 1)):
        raise SystemExit(f"{path}: transitions are not strictly increasing")
    return z


def dst_offsets(z):
    """The DST amount of each local-time type.

    TZif does not record `save`; it records only a flag. Deriving it by
    subtracting "the standard offset then in force" is the obvious way and it
    is WRONG, because a country can move its standard offset and its DST amount
    in the same reform -- CPython says so in a comment above the same function,
    and the host stand measured it: that derivation disagreed with BOTH
    libstdc++ (which reads the Rule lines and so knows the answer) and zoneinfo
    on 17 927 instants, Argentina 1992 and Morocco 2018 among them.

    This is CPython's derivation, which agrees with libstdc++: pair each DST
    type with the standard type ADJACENT to it in the transition list -- the one
    before, or failing that the one after -- and fall back to one hour for a
    type that never sits next to standard time."""
    typecnt = len(z.types)
    dstoffs = [0] * typecnt
    dst_cnt = sum(1 for t in z.types if t[1])
    found = 0
    idxs = [ti for (_tm, ti) in z.trans]

    for i in range(1, len(idxs)):
        if found == dst_cnt:
            break
        idx = idxs[i]
        if not z.types[idx][1] or dstoffs[idx]:
            continue
        utoff = z.types[idx][0]
        off = 0
        prev = idxs[i - 1]
        if not z.types[prev][1]:
            off = utoff - z.types[prev][0]
        if not off and idx < typecnt - 1 and i + 1 < len(idxs):
            nxt = idxs[i + 1]
            if z.types[nxt][1]:
                continue
            off = utoff - z.types[nxt][0]
        if off:
            found += 1
            dstoffs[idx] = off

    for i, t in enumerate(z.types):
        if t[1] and not dstoffs[i]:
            dstoffs[i] = 3600
    return dstoffs


def derive_saves(z):
    """`save` at each transition.

    ‼ The per-type amount above is only as good as the type list it is derived
    from, and `zic -b slim` does not give the same one as `zic -b fat`. Measured
    on America/Scoresbysund: slim has five local-time types and fat has nine,
    because fat keeps the future transitions and zic splits a type when the
    same (offset, flag, abbrev) triple carries two different DST amounts in two
    eras. In slim those two collapse into one `+00` type, and no single number
    is right for both -- the heuristic picked 7200 where the answer for the
    modern era is 3600, and 466 instants said so.

    So `save` is derived on the FAT tree, where zic has already separated the
    eras, and carried onto the slim transitions by instant. Every slim
    transition must exist in fat; the generator refuses to write if one does
    not. This is what the per-transition save slot was kept for, and it costs
    no bytes: the byte holding the type index has a spare nibble either way."""
    if z.recs is not None:
        return zonelines.saves_for(
            z.path, z.recs, z.trans, z.types,
            [tm for (tm, _i) in (z.fat.trans if z.fat is not None else z.trans)])

    fat = z.fat
    if fat is None:
        dstoffs = dst_offsets(z)
        return [dstoffs[ti] for (_tm, ti) in z.trans]

    fat_dstoffs = dst_offsets(fat)
    times = [tm for (tm, _ti) in fat.trans]
    saves = [fat_dstoffs[ti] for (_tm, ti) in fat.trans]

    # By INTERVAL, not by instant. Slim is not a subset of fat: where the POSIX
    # rule takes over, zic plants a transition slim needs and fat does not —
    # `GB` carries one at 1996-01-01 that fat has no counterpart for. What is
    # being asked is "what was the DST amount in force then", and the fat
    # interval containing the instant answers that whether or not the two trees
    # happen to change type at the same second.
    out = []
    for (tm, ti) in z.trans:
        if not z.types[ti][1]:
            out.append(0)
            continue
        lo, hi = 0, len(times)
        while lo < hi:
            mid = (lo + hi) // 2
            if times[mid] <= tm:
                lo = mid + 1
            else:
                hi = mid
        out.append(saves[lo - 1] if lo else 0)
    return out


def load_tree(tzdir):
    """(bodies: sha->Zone in insertion order, names: name->sha)."""
    import hashlib
    bodies, names, rep = {}, {}, {}
    for root, _d, files in os.walk(tzdir):
        for f in sorted(files):
            p = os.path.join(root, f)
            raw = open(p, "rb").read()
            if raw[:4] != b"TZif":
                continue
            rel = os.path.relpath(p, tzdir)
            h = hashlib.sha1(raw).hexdigest()
            if h not in bodies:
                z = parse_tzif(raw, rel)
                if z is None:
                    continue
                bodies[h] = z
                rep[h] = rel
            names[rel] = h
    if not bodies:
        raise SystemExit(f"{tzdir}: no TZif files — refusing to write")
    return bodies, names, rep


LINK_RE = re.compile(r"^Link\s+(\S+)\s+(\S+)")


def load_links(sources):
    """name -> target, straight from the tzdata source Link lines.

    Content identity is NOT link-ness: two zones can compile to the same bytes
    without either being a link, and [time.zone.db] asks for the real list."""
    links = {}
    for src in sources:
        for line in Path(src).read_text(errors="replace").splitlines():
            m = LINK_RE.match(line)
            if m:
                links[m.group(2)] = m.group(1)
    return links


LEAP_RE = re.compile(r"^(\d+)\s+(\d+)")
NTP_TO_UNIX = 2208988800  # seconds between 1900-01-01 and 1970-01-01


def load_leaps(path):
    """[(sys_seconds, value)] from leap-seconds.list; value is +1 or -1."""
    out, prev = [], None
    for line in Path(path).read_text(errors="replace").splitlines():
        if line.startswith("#"):
            continue
        m = LEAP_RE.match(line)
        if not m:
            continue
        ntp, tai = int(m.group(1)), int(m.group(2))
        if prev is not None:
            out.append((ntp - NTP_TO_UNIX, tai - prev))
        prev = tai
    if not out:
        raise SystemExit(f"{path}: no leap seconds parsed — refusing to write")
    return out


# ── the table ───────────────────────────────────────────────────────────────

class Pool:
    """NUL-terminated string pool with exact-match dedup."""

    def __init__(self):
        self.buf = bytearray(b"\0")   # offset 0 is the empty string
        self.at = {"": 0}

    def add(self, s):
        if s in self.at:
            return self.at[s]
        off = len(self.buf)
        self.buf += s.encode() + b"\0"
        self.at[s] = off
        return off


def varint(u):
    out = bytearray()
    while u >= 0x80:
        out.append((u & 0x7F) | 0x80)
        u >>= 7
    out.append(u)
    return bytes(out)


def zigzag(v):
    return (v << 1) ^ (v >> 63)


def build(bodies, names, links, leaps, version):
    pool = Pool()
    save_values = sorted({s for z in bodies.values() for s in derive_saves(z)})
    if len(save_values) > 16:
        raise SystemExit(f"{len(save_values)} distinct save values — the "
                         f"nibble packing this format is built on no longer "
                         f"holds; widen the transition code to two bytes")
    save_idx = {v: i for i, v in enumerate(save_values)}

    body_blobs, body_of_hash = [], {}
    for h, z in bodies.items():
        if len(z.types) > 16:
            raise SystemExit(f"{z.path}: {len(z.types)} local-time types — "
                             f"the nibble packing no longer holds")
        b = bytearray()
        b += struct.pack("<HHI", len(z.types), len(z.trans), pool.add(z.posix))
        for (utoff, isdst, ab) in z.types:
            b += struct.pack("<iHBB", utoff, pool.add(z.abbrev_of[ab]),
                             1 if isdst else 0, 0)
        prev = 0
        times = bytearray()
        for (tm, _ti) in z.trans:
            times += varint(zigzag(tm - prev))
            prev = tm
        b += struct.pack("<I", len(times))
        b += times
        for (_tm, ti), sv in zip(z.trans, derive_saves(z)):
            b.append((save_idx[sv] << 4) | ti)
        body_of_hash[h] = len(body_blobs)
        body_blobs.append(bytes(b))

    ordered = sorted(names)                     # binary search wants sorted
    index_of_name = {n: i for i, n in enumerate(ordered)}
    for n, t in links.items():
        if n in index_of_name and t not in index_of_name:
            raise SystemExit(f"link {n} -> {t}: target is not a zone")

    head_len = 44
    names_len = len(ordered) * 8
    bodytab_len = len(body_blobs) * 4
    leaps_len = len(leaps) * 16
    saves_len = 16 * 4

    off_names = head_len
    off_bodytab = off_names + names_len
    off_leaps = off_bodytab + bodytab_len
    off_saves = off_leaps + leaps_len
    off_bodies = off_saves + saves_len

    body_at, cur = [], off_bodies
    for blob in body_blobs:
        body_at.append(cur)
        cur += len(blob)
    off_pool = cur

    # the pool must be built before the header that points into it
    version_off = pool.add(version)
    name_offs = [pool.add(n) for n in ordered]

    out = bytearray()
    out += MAGIC
    out += struct.pack("<9I", version_off, len(ordered), len(body_blobs),
                       len(leaps), off_names, off_bodytab, off_leaps,
                       off_saves, off_pool)
    assert len(out) == head_len, len(out)

    for n, noff in zip(ordered, name_offs):
        target = links.get(n)
        out += struct.pack("<IHH", noff, body_of_hash[names[n]],
                           index_of_name[target] if target else NO_LINK)
    for a in body_at:
        out += struct.pack("<I", a)
    for (when, val) in leaps:
        out += struct.pack("<qii", when, val, 0)
    for i in range(16):
        out += struct.pack("<i", save_values[i] if i < len(save_values) else 0)
    for blob in body_blobs:
        out += blob
    out += pool.buf

    return bytes(out), ordered, save_values


# ── decoding it again, which is the only reason to trust it ─────────────────

class Reader:
    """A mirror of the guest reader, used to check the blob against its input.
    Deliberately independent code, not a refactor of build()."""

    def __init__(self, blob):
        if blob[:8] != MAGIC:
            raise SystemExit("blob does not start with its magic")
        (self.version_off, self.n_names, self.n_bodies, self.n_leaps,
         self.off_names, self.off_bodytab, self.off_leaps, self.off_saves,
         self.off_pool) = struct.unpack("<9I", blob[8:44])
        self.b = blob

    def _str(self, off):
        end = self.b.index(b"\0", self.off_pool + off)
        return self.b[self.off_pool + off:end].decode()

    def version(self):
        return self._str(self.version_off)

    def name(self, i):
        noff, body, link = struct.unpack(
            "<IHH", self.b[self.off_names + i * 8:self.off_names + i * 8 + 8])
        return self._str(noff), body, link

    def saves(self):
        return list(struct.unpack("<16i", self.b[self.off_saves:self.off_saves + 64]))

    def leaps(self):
        out = []
        for i in range(self.n_leaps):
            p = self.off_leaps + i * 16
            when, val, _ = struct.unpack("<qii", self.b[p:p + 16])
            out.append((when, val))
        return out

    def body(self, j):
        at, = struct.unpack("<I", self.b[self.off_bodytab + j * 4:
                                         self.off_bodytab + j * 4 + 4])
        n_types, n_trans, posix_off = struct.unpack("<HHI", self.b[at:at + 8])
        p = at + 8
        types = []
        for _ in range(n_types):
            utoff, aboff, isdst, _pad = struct.unpack("<iHBB", self.b[p:p + 8])
            types.append((utoff, bool(isdst), self._str(aboff)))
            p += 8
        tlen, = struct.unpack("<I", self.b[p:p + 4]); p += 4
        end = p + tlen
        times, val, shift, prev = [], 0, 0, 0
        while p < end:
            byte = self.b[p]; p += 1
            val |= (byte & 0x7F) << shift
            shift += 7
            if byte < 0x80:
                d = (val >> 1) ^ -(val & 1)
                prev += d
                times.append(prev)
                val, shift = 0, 0
        if shift:
            raise SystemExit("varint run ended mid-value")
        codes = self.b[end:end + n_trans]
        sv = self.saves()
        trans = [(t, c & 0x0F, sv[c >> 4]) for t, c in zip(times, codes)]
        if len(trans) != n_trans:
            raise SystemExit(f"body {j}: decoded {len(trans)} of {n_trans}")
        return types, trans, self._str(posix_off)


def verify_against_input(blob, bodies, names, links, leaps, version):
    r = Reader(blob)
    if r.version() != version:
        raise SystemExit("version string did not survive the round trip")
    if r.leaps() != leaps:
        raise SystemExit("leap seconds did not survive the round trip")

    ordered = sorted(names)
    if r.n_names != len(ordered):
        raise SystemExit("name count changed")
    prev_name = None
    for i, n in enumerate(ordered):
        got, body, link = r.name(i)
        if got != n:
            raise SystemExit(f"name {i}: {got!r} != {n!r}")
        if prev_name is not None and got <= prev_name:
            raise SystemExit(f"name index is not sorted at {got!r}")
        prev_name = got
        want_link = links.get(n)
        if want_link is None:
            if link != NO_LINK:
                raise SystemExit(f"{n}: recorded as a link and is not one")
        else:
            tgt, _b, _l = r.name(link)
            if tgt != want_link:
                raise SystemExit(f"{n}: link target {tgt!r} != {want_link!r}")

        z = bodies[names[n]]
        types, trans, posix = r.body(body)
        if posix != z.posix:
            raise SystemExit(f"{n}: POSIX footer {posix!r} != {z.posix!r}")
        want_types = [(o, d, z.abbrev_of[a]) for (o, d, a) in z.types]
        if types != want_types:
            raise SystemExit(f"{n}: types differ\n  got  {types}\n  want {want_types}")
        want_trans = [(tm, ti, sv) for (tm, ti), sv
                      in zip(z.trans, derive_saves(z))]
        if trans != want_trans:
            for k, (g, w) in enumerate(zip(trans, want_trans)):
                if g != w:
                    raise SystemExit(f"{n}: transition {k}: {g} != {w}")
            raise SystemExit(f"{n}: {len(trans)} transitions vs {len(want_trans)}")
    return r


def verify_against_zoneinfo(r, names, samples_per_zone=64):
    """An independent reader of the same database, asked the same questions.

    ‼ THIS CHECK STOPS AT THE LAST BAKED TRANSITION, and says so in its own
    count. Past that instant the answer comes from the POSIX footer, which is
    evaluated in the guest and nowhere here — a second evaluator written beside
    the first would agree with it for the same wrong reason. The footer path is
    checked in the host stand against libstdc++ AND zoneinfo, which are two
    readers that share no code with ours. A cross-check that quietly probed
    2026 would have reported agreement it never established: the first run of
    this script did exactly that, and America/Ciudad_Juarez said so."""
    try:
        from zoneinfo import ZoneInfo
    except ImportError:
        print("  (zoneinfo unavailable — cross-check skipped)", file=sys.stderr)
        return 0, 0
    import datetime as dt

    def lookup(i):
        _n, body, _l = r.name(i)
        return r.body(body)

    checked, unchecked = 0, 0
    for i in range(r.n_names):
        n, _b, _l = r.name(i)
        try:
            zi = ZoneInfo(n)
        except Exception:
            continue                     # host has an older release; skip
        types, trans, _posix = lookup(i)
        if not trans:
            unchecked += 1
            continue
        horizon = trans[-1][0]           # nothing past this is ours to answer
        unchecked += 1                   # every zone has unchecked future
        probes = []
        for (tm, _ti, _sv) in trans:
            probes += [tm - 1, tm, tm + 1]
        span = horizon - trans[0][0]
        if span > 0:
            step = max(1, span // samples_per_zone)
            probes += list(range(trans[0][0], horizon, step))
        for t in probes:
            if t > horizon or not (-2 ** 40 < t < 2 ** 40):
                continue
            # what the table says
            lo, hi = 0, len(trans)
            while lo < hi:
                mid = (lo + hi) // 2
                if trans[mid][0] <= t:
                    lo = mid + 1
                else:
                    hi = mid
            if lo == 0:
                ti = next((k for k, ty in enumerate(types) if not ty[1]), 0)
                off, _dst, ab = types[ti]
            else:
                _tm, ti, _sv = trans[lo - 1]
                off, _dst, ab = types[ti]
            when = dt.datetime.fromtimestamp(t, dt.timezone.utc).astimezone(zi)
            if int(when.utcoffset().total_seconds()) != off or when.tzname() != ab:
                raise SystemExit(
                    f"{n} at {t}: table says {off}/{ab}, zoneinfo says "
                    f"{int(when.utcoffset().total_seconds())}/{when.tzname()}")
            checked += 1
    return checked, unchecked


# ── emission ────────────────────────────────────────────────────────────────

PREAMBLE = """// boxcxx — <__bits/tzdb_table>
//
// GENERATED by tools/gen_tzdb.py from IANA tzdata %(version)s. Do not edit: the
// next run overwrites every line below, and a hand-written line here is a line
// no generator will ever check again.
//
// The reader lives in <__bits/chrono_tz>, hand-written, so that this file
// carries data and nothing else. What the bytes mean is documented there; the
// generator's own preamble records why the encoding is shaped this way, and
// what it refuses to write.
//
// %(n_names)d zone names (%(n_bodies)d distinct bodies, %(n_links)d links),
// %(n_trans)d transitions, %(n_leaps)d leap seconds, %(bytes)d bytes.
#ifndef BOXCXX_BITS_TZDB_TABLE
#define BOXCXX_BITS_TZDB_TABLE

namespace std::__boxcxx::tzdata {

inline constexpr unsigned char kTable[] = {
"""

TAIL = """};

inline constexpr unsigned kTableBytes = %(bytes)d;

}  // namespace std::__boxcxx::tzdata

#endif
"""


def emit_array(blob):
    rows = []
    for i in range(0, len(blob), 16):
        rows.append("    " + ",".join("%d" % b for b in blob[i:i + 16]) + ",")
    return "\n".join(rows) + "\n"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tzdir", required=True,
                    help="output of `zic -b slim`")
    ap.add_argument("--tzdir-fat", required=True, dest="tzdir_fat",
                    help="output of `zic -b fat`; only `save` is read from it")
    ap.add_argument("--leap", required=True)
    ap.add_argument("--links", nargs="+", required=True)
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--blob", help="also write the raw bytes here")
    a = ap.parse_args()

    bodies, names, rep = load_tree(a.tzdir)
    fat_bodies, fat_names, _fat_rep = load_tree(a.tzdir_fat)
    for h, z in bodies.items():
        fat_h = fat_names.get(rep[h])
        if fat_h is None:
            raise SystemExit(f"{rep[h]}: present in the slim tree and not in "
                             f"the fat one — refusing to write")
        z.fat = fat_bodies[fat_h]

    zone_recs = zonelines.load_zones(a.links)
    for h, z in bodies.items():
        sharing = sorted(n for n, hh in names.items() if hh == h and n in zone_recs)
        if not sharing:
            continue                       # only links point here; fat carries it
        z.recs = zone_recs[sharing[0]]
        first = derive_saves(z)
        for other in sharing[1:]:
            z.recs = zone_recs[other]
            if derive_saves(z) != first:
                raise SystemExit(f"{sharing[0]} and {other} compile to the same "
                                 f"TZif and disagree about save — refusing to write")
        z.recs = zone_recs[sharing[0]]
    links = load_links(a.links)
    links = {n: t for n, t in links.items() if n in names}
    leaps = load_leaps(a.leap)

    blob, ordered, saves = build(bodies, names, links, leaps, a.version)
    r = verify_against_input(blob, bodies, names, links, leaps, a.version)
    n_cross, n_future = verify_against_zoneinfo(r, names)

    n_trans = sum(len(z.trans) for z in bodies.values())
    text = (PREAMBLE % dict(version=a.version, n_names=len(ordered),
                            n_bodies=len(bodies), n_links=len(links),
                            n_trans=n_trans, n_leaps=len(leaps),
                            bytes=len(blob))
            + emit_array(blob)
            + TAIL % dict(bytes=len(blob)))
    Path(a.out).write_text(text)
    if a.blob:
        Path(a.blob).write_bytes(blob)

    print(f"{a.out}: tzdata {a.version} — {len(ordered)} names "
          f"({len(bodies)} bodies, {len(links)} links), {n_trans} transitions, "
          f"{len(leaps)} leap seconds, {len(saves)} save values")
    print(f"  table {len(blob)} bytes ({len(blob)/1024:.1f} KB), "
          f"source {len(text)/1024:.0f} KB")
    print(f"  round trip: every name, type and transition re-read and compared")
    print(f"  zoneinfo cross-check: {n_cross} instants agreed, "
          f"up to each zone's last baked transition")
    print(f"  NOT checked here: time past the footer in all {n_future} zones "
          f"— the host stand owns that")


if __name__ == "__main__":
    main()
