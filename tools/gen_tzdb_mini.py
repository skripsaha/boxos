#!/usr/bin/env python3
"""Generate the small zone table the TagFS door is tested with.

gen_tzdb.py bakes the real IANA database into the image. This writes a tiny
one — four names, three bodies, a made-up version — for a single purpose: to
be laid behind the `clock:tzdb` door so a test can prove that reload_tzdb()
actually loads what is there, and that a time_zone obtained before the reload
keeps answering out of the older table afterwards.

It is deliberately NOT a subset of gen_tzdb.py's writer. This emits the format
working only from the description at the top of <__bits/tzdb_read>, so if the
guest's reader accepts this table and answers correctly, two things have been
shown at once: the door works, and that description is right. A shared writer
could only ever agree with itself.

The zones are named Box/* so that no IANA release can ever collide with them,
and so a name that resolves after the reload provably could not have come from
the baked table. The leap-second list is the baked one plus a single insertion
in the year 2100 — identical where anything else looks, different where only
this test looks, so loading it cannot move utc_clock under another phase.

    tools/gen_tzdb_mini.py            # writes src/userspace/apps/tzdb_mini.h
"""

import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(ROOT, "src/userspace/boxcxx/include/std/__bits/tzdb_table")
OUT = os.path.join(ROOT, "src/userspace/apps/tzdb_mini.h")

VERSION = "boxtest1"
EXTRA_LEAP = 4102444800  # 2100-01-01T00:00:00Z, past everything anything else asks


def baked_leaps():
    """The 27 insertions in the image's own table, read out of it."""
    text = open(TABLE, encoding="utf-8").read()
    body = text.split("kTable[] = {", 1)[1].split("};", 1)[0]
    data = bytes(int(x) for x in re.findall(r"\d+", body))
    if data[:8] != b"BOXTZDB1":
        sys.exit("gen_tzdb_mini: %s does not start with the magic" % TABLE)
    n_leaps = struct.unpack_from("<I", data, 20)[0]
    off_leaps = struct.unpack_from("<I", data, 32)[0]
    out = []
    for i in range(n_leaps):
        when, value = struct.unpack_from("<qi", data, off_leaps + i * 16)
        out.append((when, value))
    return out


class Pool:
    """The string pool. Offset 0 is the empty string, and the last byte of the
    table is its terminator — the reader has no lengths, so that byte is what
    stops every walk over every name."""

    def __init__(self):
        self.blob = bytearray(b"\0")
        self.at = {"": 0}

    def add(self, s):
        if s in self.at:
            return self.at[s]
        off = len(self.blob)
        self.blob += s.encode("ascii") + b"\0"
        self.at[s] = off
        return off


def zigzag(v):
    return (v << 1) ^ (v >> 63) if v >= 0 else ((-v) << 1) - 1


def varint(v):
    out = bytearray()
    z = zigzag(v)
    while True:
        b = z & 0x7F
        z >>= 7
        if z:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def build():
    pool = Pool()

    # ── the three bodies ────────────────────────────────────────────────
    # Each: (types, transitions, posix footer). A type is (utoff, abbrev,
    # isdst); a transition is (instant, type index, save index).
    bodies = [
        # 0 — Box/Shift: one transition, so the varint path is exercised
        #     through the door and not merely the fixed-offset path.
        ([(3600, "TSTA", 0), (7200, "TSTB", 1)],
         [(1000000000, 1, 1)],
         "<TSTB>-2"),
        # 1 — Box/Test: a fixed offset no IANA zone has at this name.
        ([(19800, "+0530", 0)], [], "<+0530>-5:30"),
        # 2 — Etc/UTC: present because tzdb::current_zone falls back to it
        #     when nothing is configured, and a database without it would
        #     turn that fallback into a throw.
        ([(0, "UTC", 0)], [], "UTC0"),
    ]

    # ── the name index, sorted ──────────────────────────────────────────
    # (name, body, link target index or None)
    names = [
        ("Box/Alias", 1, "Box/Test"),
        ("Box/Shift", 0, None),
        ("Box/Test", 1, None),
        ("Etc/UTC", 2, None),
    ]
    names.sort(key=lambda e: e[0])
    index_of = {e[0]: i for i, e in enumerate(names)}

    leaps = baked_leaps() + [(EXTRA_LEAP, 1)]

    saves = [0] * 16
    saves[1] = 3600

    # ── lay the sections out ────────────────────────────────────────────
    version_off = pool.add(VERSION)
    for name, _, _ in names:
        pool.add(name)

    off_names = 44
    off_bodytab = off_names + len(names) * 8
    off_leaps = off_bodytab + len(bodies) * 4
    off_saves = off_leaps + len(leaps) * 16
    off_bodies = off_saves + 16 * 4

    body_blobs = []
    body_at = []
    cursor = off_bodies
    for types, trans, posix in bodies:
        body_at.append(cursor)
        blob = bytearray()
        blob += struct.pack("<HHI", len(types), len(trans), pool.add(posix))
        for utoff, abbrev, isdst in types:
            blob += struct.pack("<iHBB", utoff, pool.add(abbrev), isdst, 0)
        times = bytearray()
        last = 0
        for at, _, _ in trans:
            times += varint(at - last)
            last = at
        blob += struct.pack("<I", len(times))
        blob += times
        for _, ty, sv in trans:
            if ty > 15 or sv > 15:
                sys.exit("gen_tzdb_mini: a transition code is one byte")
            blob += bytes([(sv << 4) | ty])
        body_blobs.append(bytes(blob))
        cursor += len(blob)

    off_pool = cursor

    out = bytearray()
    out += b"BOXTZDB1"
    out += struct.pack("<IIII", version_off, len(names), len(bodies), len(leaps))
    out += struct.pack("<IIIII", off_names, off_bodytab, off_leaps, off_saves,
                       off_pool)
    assert len(out) == 44

    for name, body, link in names:
        target = 0xFFFF if link is None else index_of[link]
        out += struct.pack("<IHH", pool.at[name], body, target)
    for at in body_at:
        out += struct.pack("<I", at)
    for when, value in leaps:
        out += struct.pack("<qii", when, value, 0)
    for s in saves:
        out += struct.pack("<i", s)
    for blob in body_blobs:
        out += blob
    out += pool.blob

    if len(out) != off_pool + len(pool.blob):
        sys.exit("gen_tzdb_mini: the pool did not land where the header says")
    if out[-1] != 0:
        sys.exit("gen_tzdb_mini: the pool must end in a NUL")
    return bytes(out), len(names), len(bodies), len(leaps)


def main():
    table, n_names, n_bodies, n_leaps = build()

    lines = []
    for i in range(0, len(table), 16):
        lines.append("    " + ",".join(str(b) for b in table[i:i + 16]) + ",")

    with open(OUT, "w", encoding="utf-8") as f:
        f.write(
            "// GENERATED by tools/gen_tzdb_mini.py. Do not edit.\n"
            "//\n"
            "// The table the `clock:tzdb` door is tested with: %d names,\n"
            "// %d bodies, %d leap seconds, version \"%s\", %d bytes.\n"
            "//\n"
            "// Written from the format description at the top of\n"
            "// <__bits/tzdb_read> and NOT from gen_tzdb.py's writer, so that a\n"
            "// reader which accepts it has confirmed that description rather\n"
            "// than agreed with its own encoder.\n"
            "#ifndef BOXOS_TZDB_MINI_H\n"
            "#define BOXOS_TZDB_MINI_H\n"
            "\n"
            "static const unsigned char kMiniTzdb[] = {\n"
            % (n_names, n_bodies, n_leaps, VERSION, len(table)))
        f.write("\n".join(lines))
        f.write("\n};\n\nstatic const unsigned kMiniTzdbBytes = %d;\n"
                "\n#endif\n" % len(table))

    print("wrote %s: %d bytes, %d names, %d bodies, %d leaps, version %s"
          % (OUT, len(table), n_names, n_bodies, n_leaps, VERSION))


if __name__ == "__main__":
    main()
