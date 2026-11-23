"""selftest_headers — X1, X2, X3: the format's numbers, its owners and the
book's alphabet held against the C headers in src/, read as text."""

import importlib.util
import os
import re

from . import binder, bookread, ondisk, stitch, tally

# ---------------------------------------------------------------------------
# X1: the constants and the struct layouts, held against the headers
# ---------------------------------------------------------------------------

_DEFINE = re.compile(r"^\s*#define\s+(\w+)\s+(.+?)\s*(?://.*|/\*.*)?$", re.M)
_TYPES = {"uint8_t": 1, "char": 1, "uint16_t": 2, "uint32_t": 4, "uint64_t": 8}


def _Value(text, defines):
    text = text.strip()
    m = re.fullmatch(r"'(\\0|.)'", text)
    if m:
        return 0 if m.group(1) == "\\0" else ord(m.group(1))
    m = re.fullmatch(r"\(\s*1U?\s*<<\s*(\d+)\s*\)", text)
    if m:
        return 1 << int(m.group(1))
    m = re.fullmatch(r"\(\s*(\d+)[uU]?\s*\*\s*(\d+)[uU]?\s*\)", text)
    if m:
        return int(m.group(1)) * int(m.group(2))
    m = re.fullmatch(r"\(\s*(\w+)\s*-\s*(\w+)\s*\)", text)
    if m:
        a, b = _Value(m.group(1), defines), _Value(m.group(2), defines)
        return None if a is None or b is None else a - b
    m = re.fullmatch(r"(0[xX][0-9a-fA-F]+|\d+)[uUlL]*", text)
    if m:
        return int(m.group(1), 0)
    return defines.get(text)


def Defines(text, known=None):
    """Every #define in `text` whose value can be read: a number, a char, a
    shift, a product, a difference, or the name of one defined above it here
    or in `known` — the headers read before it, so an alias resolves to its
    owner."""
    out = {}
    for name, value in _DEFINE.findall(text):
        v = _Value(value, dict(known or {}, **out))
        if v is not None:
            out[name] = v
    return out


def Structs(text, defines):
    """{name: [(field, offset, size)]} for every packed typedef struct."""
    out, sizes = {}, dict(_TYPES)
    for body, name in re.findall(r"typedef struct (?:__attribute__\(\(packed\)\)|__packed)\s*\{(.*?)\}\s*(\w+);",
                                 text, re.S):
        fields, at = [], 0
        for typ, star, field, count in re.findall(
                r"^\s*(?:struct\s+)?(\w+)\s*(\*?)\s*(\w+)\s*(?:\[(\w+)\])?\s*;", body, re.M):
            size = 8 if star else sizes.get(typ)
            if size is None:
                break
            n = 1 if not count else (int(count, 0) if count[0].isdigit() else defines.get(count))
            if n is None:
                break
            fields.append((field, at, size * n))
            at += size * n
        out[name] = fields
        sizes[name] = at
    return out


def ConstantsCheck(src):
    """The format's numbers held against the headers."""
    inc, fs = os.path.join(src, "include"), os.path.join(src, "kernel", "tagfs")
    paths = {"deed": os.path.join(inc, "volume_deed.h"), "pen": os.path.join(inc, "deed_pen.h"),
             "consts": os.path.join(fs, "tagfs_constants.h"),
             "ground": os.path.join(src, "kernel", "core", "boardroom", "ground.c"),
             "groundh": os.path.join(src, "kernel", "core", "boardroom", "ground.h"),
             "boardroom": os.path.join(src, "kernel", "core", "boardroom", "boardroom.h")}
    if not all(os.path.exists(p) for p in paths.values()):
        return None, "self-test needs the source tree beside the tool (src/ next to tools/)"
    text = {k: open(p, encoding="utf-8", errors="replace").read() for k, p in paths.items()}
    d = {}
    for k in text:
        d.update(Defines(text[k], d))
    wrong, n = [], 0

    def Eq(name, have, want):
        nonlocal n
        n += 1
        if have != want:
            wrong.append("%s: the header says %r and ondisk.py %r" % (name, want, have))

    Eq("VOLUME_DEED_MAGIC", ondisk.DEED_MAGIC, bytes(d["VOLUME_DEED_MAGIC_%u" % i] for i in range(8)))
    for py, c in (("SECTOR", "VOLUME_DEED_SECTOR_BYTES"), ("DEED_ROLE_HEAD", "VOLUME_DEED_ROLE_HEAD"),
                  ("DEED_ROLE_TAIL", "VOLUME_DEED_ROLE_TAIL"), ("STAMP_GEOMETRY", "VOLUME_STAMP_GEOMETRY"),
                  ("STAMP_BORN", "VOLUME_STAMP_BORN"), ("STAMP_BOOT", "VOLUME_STAMP_BOOT"),
                  ("STAMP_PACKED", "VOLUME_STAMP_PACKED"), ("STAMP_FRAME", "VOLUME_STAMP_FRAME"),
                  ("FRAME_SPINE_SLOTS", "VOLUME_FRAME_SPINE_SLOTS"),
                  ("SECTORS_PER_BLOCK", "VOLUME_BLOCK_SECTORS"), ("BLOCK", "TAGFS_BLOCK_SIZE"),
                  ("DEED_SECTORS", "DEED_PEN_SECTORS"), ("BLOCK", "VOLUME_BLOCK_BYTES"),
                  ("FILE_ACTIVE", "TAGFS_FILE_ACTIVE"), ("FILE_UNDONE", "TAGFS_FILE_UNDONE"), ("FILE_DELETED", "TAGFS_FILE_DELETED"),
                  ("FILE_ROLE_MASK", "TAGFS_FILE_ROLE_MASK"), ("FILE_ROLE_SYSTEM", "TAGFS_FILE_ROLE_SYSTEM"),
                  ("FILE_ROLE_BOOT", "TAGFS_FILE_ROLE_BOOT"), ("FILE_ROLE_KERNEL", "TAGFS_FILE_ROLE_KERNEL"),
                  ("FILE_ARRIVING", "TAGFS_FILE_ARRIVING"),
                  ("MBR_TYPE_BOXOS", "GROUND_MBR_TYPE"), ("MBR_TYPE_PROTECTIVE", "GROUND_MBR_TYPE_PROTECTIVE"),
                  ("GPT_ARRAY_MAX_BYTES", "GPT_ARRAY_MAX_BYTES"), ("LOGICAL_MOST", "BOARDROOM_LOGICAL_MOST"),
                  ("GROUND_MAX_PER_MEDIUM", "GROUND_MAX_PER_MEDIUM")):
        Eq(c, getattr(ondisk, py), d.get(c))
    guid = re.search(r"g_boxos_type_guid\[16\]\s*=\s*\{(.*?)\}", text["ground"], re.S)
    Eq("g_boxos_type_guid", ondisk.BOXOS_TYPE_GUID,
       bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", guid.group(1))) if guid else None)
    structs = Structs(text["deed"], d)
    for cname, fields in (("VolumeDeed", ondisk.VOLUME_DEED), ("VolumeGeometry", ondisk.VOLUME_GEOMETRY),
                          ("VolumeBoot", ondisk.VOLUME_BOOT), ("VolumeBorn", ondisk.VOLUME_BORN),
                          ("VolumePacked", ondisk.VOLUME_PACKED), ("VolumeFrame", ondisk.VOLUME_FRAME)):
        c_fields = {f: (o, s) for f, o, s in structs.get(cname, [])}
        for field, off, size in fields.Offsets():
            Eq("%s.%s" % (cname, field), (off, size), c_fields.get(field))
    return wrong, n


# ---------------------------------------------------------------------------
# X2: the owners of the volume's ground, held against the header
# ---------------------------------------------------------------------------


def OwnersCheck(src):
    """What is wrong with ground_owners.py: not what tools/gen_ground_owners.py
    writes out of src/include/ground_owner.h today. The owners are held to the
    landings that hear them claim by selftest.BookOwnersCheck."""
    gen = os.path.join(os.path.dirname(src), "tools", "gen_ground_owners.py")
    spec = importlib.util.spec_from_file_location("gen_ground_owners", gen)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    with open(module.OUT) as fh:
        if fh.read() != module.Text(module.Owners()):
            return ["%s is stale against %s — run tools/gen_ground_owners.py" % (module.OUT, module.HEADER)]
    return []


# ---------------------------------------------------------------------------
# X3: the book's alphabet and the block digest, held against the headers
# ---------------------------------------------------------------------------


def AlphabetCheck(src):
    """What is wrong with bookread.py against volume_book.h and volume_digest.h,
    and with tally.py against volume_tally.h, read the way X1 reads its
    headers, and how many things were held — and the binder's two rules of
    writing every writer keeps alike (volume_tally.h: the cut of a file into
    Parcels, §16.1 п.7, and the most digests an element carries, §16.3 п.1)."""
    inc = os.path.join(src, "include")
    paths = [os.path.join(inc, n) for n in ("volume_deed.h", "volume_book.h", "volume_digest.h",
                                            "volume_tally.h", "volume_ribbon.h")]
    if not all(os.path.exists(p) for p in paths):
        return None, "self-test needs the source tree beside the tool (src/ next to tools/)"
    text, d = [open(p, encoding="utf-8", errors="replace").read() for p in paths], {}
    for t in text:
        d.update(Defines(t, d))
    wrong, n = bookread.AlphabetCheck(d, Structs(text[1], d), text[1])
    more, m = tally.TallyCheck(d)
    wrong += more
    for py, c in ((binder.PARCEL_STRIPS_MOST, "PARCEL_STRIPS_MOST"), (binder.DIGEST_ITEM_MOST, "DIGEST_ITEM_MOST")):
        if py != d.get(c):
            wrong.append("%s: volume_tally.h says %r and binder.py %r" % (c, d.get(c), py))
    ribbon, k = stitch.RibbonCheck(d)
    return wrong + ribbon, n + m + 2 + k
