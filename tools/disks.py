#!/usr/bin/env python3
"""
disks.py — a disk's partition table on the host, read by the rules the
kernel's DiskTable reads it by (src/kernel/core/boardroom/disk_table.c), laid
for a gate to hand the machine, and every cut of a change of it judged.

    disks.py read IMG [--block N] [--json]
        the table: its state, what its copies say of each other, every
        partition and every free run, as DiskTable tells them
    disks.py lay IMG SIZE [--block N] [--part KIND:SIZE[:NAME]]... [--fill]
        a fresh image of SIZE (K, M, G — binary) with a GPT laid as a
        partitioning tool lays one — 128 entries, partitions from 1 MiB on
        the grain — KIND one of esp, msr, windows, recovery, linux, boxos;
        --fill writes every partition full of bytes its own
    disks.py damage IMG first|far [--block N]
        one copy of its GPT's header zeroed, as a dead block takes it
    disks.py grow IMG SIZE
        the image made larger, its table left as it was: a table made for a
        smaller disk
    disks.py cuts BEFORE AFTER [--block N]
        every medium a cut could leave of the change that made AFTER of
        BEFORE — the kernel writes the far copy (its array, then its header)
        and flushes, then the first copy and flushes, then sector 0 and
        flushes; a medium may land any of one flush's writes and not the
        rest, and tear the last by its sectors — each read as DiskTable reads
        it, and held to be the old table or the new, whole
    disks.py cut BEFORE AFTER OUT WRITES [--block N]
        the medium a cut leaves after the change's first WRITES writes, in
        the kernel's order, landed whole — for the kernel to read as the host
        does

SIZE is K, M or G, binary. Exit 0 when what was asked holds.
"""
import argparse
import binascii
import json
import os
import struct
import sys
import uuid

SECTOR = 512
GRAIN = 1024 * 1024
ENTRIES, ENTRY_BYTES = 128, 128

KINDS = {
    'boxos': uuid.UUID('cf8ae49a-d26a-4959-9a6f-71c932e0c9eb'),
    'esp': uuid.UUID('c12a7328-f81f-11d2-ba4b-00a0c93ec93b'),
    'windows': uuid.UUID('ebd0a0a2-b9e5-4433-87c0-68b6b72699c7'),
    'msr': uuid.UUID('e3c9e316-0b5c-4db8-817d-f92df00215ae'),
    'recovery': uuid.UUID('de94bba4-06d1-4d40-a16a-bfd50179d6ac'),
    'linux': uuid.UUID('0fc63daf-8483-4772-8e79-3d69d8477de4'),
}
KIND_OF = {g.bytes_le: k for k, g in KINDS.items()}

FIRST_DAMAGED, FAR_DAMAGED, DIFFER, FAR_ELSEWHERE = 0x01, 0x02, 0x04, 0x08


def crc32(b):
    return binascii.crc32(b) & 0xFFFFFFFF


def size_of(text):
    units = {'K': 1 << 10, 'M': 1 << 20, 'G': 1 << 30}
    return int(text[:-1]) * units[text[-1].upper()] if text[-1].upper() in units else int(text)


class Medium:
    """An image read and written in its blocks. A clone shares the image and
    keeps the blocks written to it since apart, so a medium a cut could leave
    costs the blocks it changed and not the image."""

    def __init__(self, path, block, data=None):
        self.block = block
        self.data = bytearray(open(path, 'rb').read()) if data is None else data
        self.blocks = len(self.data) // block
        self.over = {}

    def clone(self):
        m = Medium(None, self.block, self.data)
        m.over = dict(self.over)
        return m

    def get(self, at, count=1):
        return b''.join(self.over.get(b) or bytes(self.data[b * self.block:(b + 1) * self.block])
                        for b in range(at, at + count))

    def put(self, at, raw):
        """`raw` from block `at` on; a last block it does not fill keeps the
        rest of what it held, as a write torn by its sectors leaves it."""
        for i in range(0, len(raw), self.block):
            piece = raw[i:i + self.block]
            if len(piece) < self.block:
                piece = piece + self.get(at + i // self.block)[len(piece):]
            self.over[at + i // self.block] = bytes(piece)

    def sector0(self):
        return self.get(0)[:SECTOR]

    def save(self, path):
        with open(path, 'r+b') as fh:
            for b, raw in sorted(self.over.items()):
                fh.seek(b * self.block)
                fh.write(raw)


# ── reading, as disk_table.c reads ─────────────────────────────────────────

def copy_read(m, at):
    """One copy of a GPT whose header lies in block `at`: a dict, or None
    when it does not hold (DiskGptCopyRead)."""
    if at <= 0 or at >= m.blocks:
        return None
    h = bytearray(m.get(at))
    if h[0:8] != b'EFI PART':
        return None
    size = struct.unpack_from('<I', h, 12)[0]
    if size < 92 or size > m.block:
        return None
    want = struct.unpack_from('<I', h, 16)[0]
    h[16:20] = b'\0' * 4
    if crc32(bytes(h[:size])) != want or struct.unpack_from('<Q', h, 24)[0] != at:
        return None
    alt, first, last = struct.unpack_from('<QQQ', h, 32)
    entry_lba, count, entry_bytes, array_crc = struct.unpack_from('<QIII', h, 72)
    if entry_bytes < 128 or entry_bytes & (entry_bytes - 1) or not count or count * entry_bytes > 128 * 1024:
        return None
    if first > last or last >= m.blocks:
        return None
    array_blocks = (count * entry_bytes + m.block - 1) // m.block
    if entry_lba + array_blocks > m.blocks:
        return None
    array = m.get(entry_lba, array_blocks)[:count * entry_bytes]
    if crc32(array) != array_crc:
        return None
    return dict(header=at, alt=alt, first_usable=first, last_usable=last, guid=bytes(h[56:72]),
                entry_lba=entry_lba, count=count, entry_bytes=entry_bytes, crc=array_crc, array=array)


def agree(a, b):
    return all(a[k] == b[k] for k in ('guid', 'first_usable', 'last_usable', 'count', 'entry_bytes', 'crc'))


def name_of(units):
    return units.decode('utf-16-le', 'replace').split('\0')[0]


def table_read(m):
    """The table as DiskTable reads it (DiskTableReadMedium)."""
    s0 = m.sector0()
    t = dict(state='none', copies=0, parts=[], free=[])
    if s0[510:512] != b'\x55\xaa':
        return t
    if not any(s0[446 + 16 * i + 4] == 0xEE for i in range(4)):
        t['state'] = 'mbr'
        for i in range(4):
            e = s0[446 + 16 * i:462 + 16 * i]
            start, count = struct.unpack_from('<II', e, 8)
            if e[4] and count and start + count <= m.blocks:
                t['parts'].append(dict(entry=i, kind='mbr:%02x' % e[4], first=start, last=start + count - 1))
        t['parts'].sort(key=lambda p: p['first'])
        return t
    first = copy_read(m, 1)
    read = first
    if first:
        at = m.blocks - 1
        if first['alt'] != m.blocks - 1:
            t['copies'] |= FAR_ELSEWHERE
            at = first['alt']
        far = copy_read(m, at)
        if not far:
            t['copies'] |= FAR_DAMAGED
        elif not agree(first, far):
            t['copies'] |= DIFFER
    else:
        far = copy_read(m, m.blocks - 1)
        if not far:
            t['state'] = 'gpt-lost'
            return t
        t['copies'] |= FIRST_DAMAGED
        read = far
    t['state'] = 'gpt'
    t.update(guid=read['guid'].hex(), first_usable=read['first_usable'], last_usable=read['last_usable'])
    for i in range(read['count']):
        e = read['array'][i * read['entry_bytes']:(i + 1) * read['entry_bytes']]
        if not any(e[:16]):
            continue
        lo, hi = struct.unpack_from('<QQ', e, 32)
        if hi < lo or lo < read['first_usable'] or hi > read['last_usable']:
            continue
        t['parts'].append(dict(entry=i, kind=KIND_OF.get(bytes(e[:16]), 'other'), first=lo, last=hi,
                               unique=bytes(e[16:32]).hex(), name=name_of(bytes(e[56:128]))))
    t['parts'].sort(key=lambda p: p['first'])
    grain = max(GRAIN // m.block, 1)
    at = read['first_usable']
    for i in range(len(t['parts']) + 1):
        until = t['parts'][i]['first'] if i < len(t['parts']) else read['last_usable'] + 1
        start = (at + grain - 1) // grain * grain
        if until > start and until - start >= grain:
            t['free'].append(dict(first=start, blocks=until - start))
        if i < len(t['parts']):
            at = max(at, t['parts'][i]['last'] + 1)
    return t


# ── laying, as a partitioning tool lays ────────────────────────────────────

def gpt_lay(m, parts):
    """A GPT over all of `m`: parts [(kind, blocks, name)], from 1 MiB on."""
    array_blocks = (ENTRIES * ENTRY_BYTES + m.block - 1) // m.block
    first_usable, far = 2 + array_blocks, m.blocks - 1
    last_usable = far - array_blocks - 1
    grain = GRAIN // m.block
    array = bytearray(ENTRIES * ENTRY_BYTES)
    at = grain
    for i, (kind, blocks, name) in enumerate(parts):
        e = memoryview(array)[i * ENTRY_BYTES:(i + 1) * ENTRY_BYTES]
        e[0:16] = KINDS[kind].bytes_le
        e[16:32] = uuid.uuid4().bytes_le
        last = min(at + blocks - 1, last_usable)
        struct.pack_into('<QQQ', e, 32, at, last, 0)
        raw = name.encode('utf-16-le')[:72]
        e[56:56 + len(raw)] = raw
        at = (last + 1 + grain - 1) // grain * grain
    array = bytes(array)
    guid = uuid.uuid4().bytes_le

    def header(my, alt, entries):
        h = bytearray(m.block)
        h[0:8] = b'EFI PART'
        struct.pack_into('<II', h, 8, 0x00010000, 92)
        struct.pack_into('<QQQQ', h, 24, my, alt, first_usable, last_usable)
        h[56:72] = guid
        struct.pack_into('<QIII', h, 72, entries, ENTRIES, ENTRY_BYTES, crc32(array))
        struct.pack_into('<I', h, 16, crc32(bytes(h[:92])))
        return bytes(h)

    mbr = bytearray(m.block)
    mbr[446 + 1:446 + 4] = b'\x00\x02\x00'
    mbr[446 + 4] = 0xEE
    mbr[446 + 5:446 + 8] = b'\xff\xff\xff'
    struct.pack_into('<II', mbr, 446 + 8, 1, min(m.blocks - 1, 0xFFFFFFFF))
    mbr[510:512] = b'\x55\xaa'
    m.put(0, bytes(mbr))
    m.put(1, header(1, far, 2))
    m.put(2, array)
    m.put(far - array_blocks, array)
    m.put(far, header(far, 1, far - array_blocks))


# ── the cuts of a change ───────────────────────────────────────────────────

def new_esps(before, after):
    """The ESPs AFTER's table names that BEFORE's does not: [(first, blocks)]."""
    was = {(p['first'], p['last']) for p in table_read(before)['parts'] if p['kind'] == 'esp'}
    return [(p['first'], p['last'] - p['first'] + 1) for p in table_read(after)['parts']
            if p['kind'] == 'esp' and (p['first'], p['last']) not in was]


def regions(before, after):
    """The writes of the change from `before` to `after`, in the kernel's order
    (disk_table_write.c DiskEspLaid, then DiskPlanWrite), by flush:
    [[(first block, blocks)]] — a new ESP's FAT32 first, in free space yet."""
    a = copy_read(after, 1)
    if not a:
        raise SystemExit('cuts: AFTER holds no first copy of a GPT — no change the kernel makes ends so')
    far = copy_read(after, a['alt'])
    array_blocks = (a['count'] * a['entry_bytes'] + after.block - 1) // after.block
    epochs = [[esp] for esp in new_esps(before, after)]
    epochs += [[(far['entry_lba'], array_blocks), (far['header'], 1)], [(a['entry_lba'], array_blocks), (1, 1)]]
    if before.get(0) != after.get(0):
        epochs.append([(0, 1)])
    return epochs


def same(t, u):
    keys = ('entry', 'kind', 'first', 'last', 'unique')
    return t['state'] == u['state'] and [tuple(p.get(k) for k in keys) for p in t['parts']] == \
        [tuple(p.get(k) for k in keys) for p in u['parts']]


def cuts(before, after):
    old, new = table_read(before), table_read(after)
    epochs = regions(before, after)
    esps = new_esps(before, after)
    ratio = before.block // SECTOR
    built = bad = 0
    for e, writes in enumerate(epochs):
        base = before.clone()
        for w in epochs[:e]:
            for first, count in w:
                base.put(first, after.get(first, count))
        for mask in range(1 << len(writes)):
            landed = [w for i, w in enumerate(writes) if mask >> i & 1]
            torn = [w for i, w in enumerate(writes) if not mask >> i & 1]
            tears = [(None, 0)] + [(w, s) for w in torn for s in range(1, w[1] * ratio, max(1, w[1] * ratio // 16))]
            for w, sectors in tears:
                m = base.clone()
                for first, count in landed:
                    m.put(first, after.get(first, count))
                if w:
                    m.put(w[0], after.get(w[0], w[1])[:sectors * SECTOR])
                t = table_read(m)
                built += 1
                if same(t, new) and any(m.get(f, 1) != after.get(f, 1) for f, _n in esps):
                    bad += 1
                    print('cuts: flush %u, landed %s, torn %s at sector %u — the table names an ESP whose FAT32 '
                          'is not yet there' % (e + 1, landed, w, sectors))
                elif not (same(t, old) or same(t, new)):
                    bad += 1
                    print('cuts: flush %u, landed %s, torn %s at sector %u — read as %s with %u partition(s), '
                          'neither the old table nor the new' % (e + 1, landed, w, sectors, t['state'], len(t['parts'])))
    print('cuts: %u medium/media a cut could leave, %u read as neither the old table nor the new' % (built, bad))
    return bad == 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    r = sub.add_parser('read')
    r.add_argument('img')
    r.add_argument('--block', type=int, default=512)
    r.add_argument('--json', action='store_true')
    lay = sub.add_parser('lay')
    lay.add_argument('img')
    lay.add_argument('size')
    lay.add_argument('--block', type=int, default=512)
    lay.add_argument('--part', action='append', default=[])
    lay.add_argument('--fill', action='store_true')
    d = sub.add_parser('damage')
    d.add_argument('img')
    d.add_argument('which', choices=('first', 'far'))
    d.add_argument('--block', type=int, default=512)
    g = sub.add_parser('grow')
    g.add_argument('img')
    g.add_argument('size')
    c = sub.add_parser('cuts')
    c.add_argument('before')
    c.add_argument('after')
    c.add_argument('--block', type=int, default=512)
    one = sub.add_parser('cut')
    one.add_argument('before')
    one.add_argument('after')
    one.add_argument('out')
    one.add_argument('writes', type=int)
    one.add_argument('--block', type=int, default=512)
    a = ap.parse_args()

    if a.cmd == 'read':
        t = table_read(Medium(a.img, a.block))
        if a.json:
            print(json.dumps(t))
        else:
            print('%s, copies 0x%02x' % (t['state'], t['copies']))
            for p in t['parts']:
                print('  entry %u  %s  %u..%u  %s' % (p['entry'], p['kind'], p['first'], p['last'], p.get('name', '')))
            for f in t['free']:
                print('  free  %u from %u' % (f['blocks'], f['first']))
        return 0
    if a.cmd == 'lay':
        with open(a.img, 'wb') as fh:
            fh.truncate(size_of(a.size))
        m = Medium(a.img, a.block)
        parts = []
        for spec in a.part:
            kind, size, *name = spec.split(':')
            parts.append((kind, size_of(size) // a.block, name[0] if name else kind))
        gpt_lay(m, parts)
        m.save(a.img)
        if a.fill:
            with open(a.img, 'r+b') as fh:
                for p in table_read(m)['parts']:
                    fh.seek(p['first'] * a.block)
                    fh.write(struct.pack('<I', p['entry']) * ((p['last'] - p['first'] + 1) * a.block // 4))
        return 0
    if a.cmd == 'damage':
        m = Medium(a.img, a.block)
        m.put(1 if a.which == 'first' else m.blocks - 1, bytes(m.block))
        m.save(a.img)
        return 0
    if a.cmd == 'grow':
        with open(a.img, 'r+b') as fh:
            fh.truncate(size_of(a.size))
        return 0
    if a.cmd == 'cut':
        before, after = Medium(a.before, a.block), Medium(a.after, a.block)
        writes = [w for epoch in regions(before, after) for w in epoch]
        with open(a.out, 'wb') as fh:
            fh.write(before.data)
        for first, count in writes[:a.writes]:
            before.put(first, after.get(first, count))
        before.save(a.out)
        return 0
    return 0 if cuts(Medium(a.before, a.block), Medium(a.after, a.block)) else 1


if __name__ == '__main__':
    sys.exit(main())
