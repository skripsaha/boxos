"""selftest_headers — X1, X2, X3: the format's numbers, its owners and the
book's alphabet held against the C headers in src/, read as text."""

import importlib.util
import os
import re

from . import binder, bookread, ondisk, stitch, tally
from .ground_owners import GROUND_OWNERS

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
    """The format's numbers held against the headers.

    ‼ THE KERNEL'S HEADERS ARE READ LAST, so theirs is the word ondisk.py is
    held to. The UEFI loader keeps copies of some of the same numbers
    (tagfs_boot.h), and when it was read last its copy was what got checked —
    a loader that agreed with ondisk.py hid a kernel that did not. Now the
    loader is read first, and each number it shares with the kernel is held to
    the kernel's on its own line."""
    inc, fs = os.path.join(src, "include"), os.path.join(src, "kernel", "tagfs")
    paths = {"boot": os.path.join(src, "boot", "uefi", "tagfs_boot.h"),
             "magic": os.path.join(inc, "boxos_magic.h"), "deed": os.path.join(inc, "volume_deed.h"),
             "ledger": os.path.join(inc, "volume_ledger.h"), "pen": os.path.join(inc, "deed_pen.h"),
             "tagfs": os.path.join(fs, "tagfs.h"), "consts": os.path.join(fs, "tagfs_constants.h"),
             "cow": os.path.join(fs, "cow", "cow.h"), "book": os.path.join(fs, "disk_book", "disk_book.h"),
             "pool": os.path.join(fs, "metadata_pool", "meta_pool_internal.h"),
             "plot": os.path.join(fs, "plot", "plot_width.h"),
             "ground": os.path.join(src, "kernel", "core", "boardroom", "ground.c"),
             "groundh": os.path.join(src, "kernel", "core", "boardroom", "ground.h")}
    if not all(os.path.exists(p) for p in paths.values()):
        return None, "self-test needs the source tree beside the tool (src/ next to tools/)"
    text = {k: open(p, encoding="utf-8", errors="replace").read() for k, p in paths.items()}
    loader, kernel = Defines(text["boot"]), {}
    for k in text:
        if k != "boot":
            kernel.update(Defines(text[k], kernel))
    d = dict(loader, **kernel)
    wrong, n = [], 0

    def Eq(name, have, want):
        nonlocal n
        n += 1
        if have != want:
            wrong.append("%s: the header says %r and ondisk.py %r" % (name, want, have))

    for name in sorted(set(loader) & set(kernel)):
        n += 1
        if loader[name] != kernel[name]:
            wrong.append("%s: tagfs_boot.h says %r and the kernel's headers %r"
                         % (name, loader[name], kernel[name]))

    magic = lambda p: bytes(d["%s_MAGIC_%u" % (p, i)] for i in range(8))
    Eq("VOLUME_DEED_MAGIC", ondisk.DEED_MAGIC, magic("VOLUME_DEED"))
    Eq("VOLUME_LEDGER_MAGIC", ondisk.LEDGER_MAGIC, magic("VOLUME_LEDGER"))
    for py, c in (("SECTOR", "VOLUME_DEED_SECTOR_BYTES"), ("DEED_ROLE_HEAD", "VOLUME_DEED_ROLE_HEAD"),
                  ("DEED_ROLE_TAIL", "VOLUME_DEED_ROLE_TAIL"), ("STAMP_GEOMETRY", "VOLUME_STAMP_GEOMETRY"),
                  ("STAMP_LAYOUT", "VOLUME_STAMP_LAYOUT"), ("STAMP_BORN", "VOLUME_STAMP_BORN"),
                  ("STAMP_BOOT", "VOLUME_STAMP_BOOT"), ("STAMP_PACKED", "VOLUME_STAMP_PACKED"),
                  ("STAMP_FRAME", "VOLUME_STAMP_FRAME"), ("FRAME_SPINE_SLOTS", "VOLUME_FRAME_SPINE_SLOTS"),
                  ("SECTORS_PER_BLOCK", "VOLUME_BLOCK_SECTORS"), ("BLOCK", "TAGFS_BLOCK_SIZE"),
                  ("DEED_SECTORS", "DEED_PEN_SECTORS"), ("LEDGER_COPIES", "VOLUME_LEDGER_COPIES"),
                  ("REGISTRY_MAGIC", "TAGFS_REGISTRY_MAGIC"), ("FILETBL_MAGIC", "TAGFS_FILETBL_MAGIC"),
                  ("MPOOL_MAGIC", "TAGFS_MPOOL_MAGIC"), ("BLOCK", "VOLUME_BLOCK_BYTES"),
                  ("REGISTRY_DATA", "TAGFS_REGISTRY_DATA_SIZE"), ("MPOOL_DATA", "TAGFS_MPOOL_DATA_SIZE"),
                  ("FTABLE_PER_BLOCK", "TAGFS_FTABLE_PER_BLOCK"), ("MAX_TAG_ID", "TAGFS_V1_TAG_ID_MOST"),
                  ("FILE_ACTIVE", "TAGFS_FILE_ACTIVE"),
                  ("TAG_FLAG_HAS_VALUE", "TAGFS_TAG_FLAG_HAS_VALUE"), ("TAG_FLAG_SYSTEM", "TAGFS_TAG_FLAG_SYSTEM"),
                  ("COW_MANIFEST_MAGIC", "COW_MANIFEST_MAGIC"), ("COW_MANIFEST_MAX", "COW_MANIFEST_MAX"),
                  ("DISKBOOK_MAGIC", "DISK_BOOK_MAGIC"), ("DISKBOOK_SB_MAGIC", "DISK_BOOK_SB_MAGIC"),
                  ("DISKBOOK_VERSION", "DISK_BOOK_VERSION"), ("DISKBOOK_ENTRY_BYTES", "DISK_BOOK_ENTRY_SIZE"),
                  ("DISKBOOK_SECTORS_PER_ENTRY", "DISK_BOOK_SECTORS_PER_ENTRY"),
                  ("DISKBOOK_CAPACITY", "DISK_BOOK_CAPACITY"), ("DISKBOOK_TYPE_REDIRECT", "DISK_BOOK_TYPE_REDIRECT"),
                  ("RECORD_HEADER", "RECORD_HEADER_SIZE"), ("MPOOL_BLOCK_HEADER", "MPOOL_BLOCK_HEADER"),
                  ("RECORD_CRC_AT", "RECORD_CRC_OFFSET"), ("MBR_TYPE_BOXOS", "GROUND_MBR_TYPE"),
                  ("MBR_TYPE_PROTECTIVE", "GROUND_MBR_TYPE_PROTECTIVE"),
                  ("GPT_ARRAY_MAX_BYTES", "GPT_ARRAY_MAX_BYTES"), ("GPT_BLOCK_MAX_BYTES", "GPT_BLOCK_MAX_BYTES"),
                  ("GROUND_MAX_PER_MEDIUM", "GROUND_MAX_PER_MEDIUM"),
                  ("TUSE_MAGIC", "USE_CHAIN_MAGIC"), ("TUSE_TEXT", "USE_CHAIN_TEXT")):
        Eq(c, getattr(ondisk, py), d.get(c))
    guid = re.search(r"g_boxos_type_guid\[16\]\s*=\s*\{(.*?)\}", text["ground"], re.S)
    Eq("g_boxos_type_guid", ondisk.BOXOS_TYPE_GUID,
       bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", guid.group(1))) if guid else None)
    rec = dict((f, o) for f, o, _s in ondisk.META_RECORD.Offsets())
    for c, field in (("META_RECORD_LEN_OFF", "record_len"), ("META_RECORD_FILE_ID_OFF", "file_id"),
                     ("META_RECORD_FLAGS_OFF", "flags"), ("META_RECORD_SIZE_OFF", "size"),
                     ("META_RECORD_TAGCOUNT_OFF", "tag_count"), ("META_RECORD_EXTCOUNT_OFF", "extent_count"),
                     ("META_RECORD_NAMELEN_OFF", "name_len"), ("META_RECORD_CRC_OFF", "crc16")):
        Eq(c, rec[field], d.get(c))
    Eq("META_RECORD_VARDATA_OFF", ondisk.META_RECORD.size, d.get("META_RECORD_VARDATA_OFF"))
    _StructsCheck(text, d, Eq)
    return wrong, n


def _StructsCheck(text, d, Eq):
    """X1's struct layouts: every field of every on-disk struct ondisk.py
    describes, held to the header's offset and size through `Eq`."""
    structs = {}
    for k in ("deed", "ledger", "tagfs", "plot", "cow", "book"):
        structs.update(Structs(text[k], d))
    for cname, fields in (("VolumeDeed", ondisk.VOLUME_DEED), ("VolumeGeometry", ondisk.VOLUME_GEOMETRY),
                          ("VolumeLayout", ondisk.VOLUME_LAYOUT), ("VolumeBoot", ondisk.VOLUME_BOOT),
                          ("VolumeBorn", ondisk.VOLUME_BORN), ("VolumePacked", ondisk.VOLUME_PACKED),
                          ("VolumeFrame", ondisk.VOLUME_FRAME),
                          ("VolumeLedger", ondisk.VOLUME_LEDGER), ("TagRegistryBlock", ondisk.REGISTRY_BLOCK),
                          ("FileTableBlock", ondisk.FILE_TABLE_BLOCK), ("FileTableEntry", ondisk.FILE_TABLE_ENTRY),
                          ("MetaPoolBlock", ondisk.META_POOL_BLOCK), ("FileExtent", ondisk.FILE_EXTENT),
                          ("CowSnapshotDisk", ondisk.COW_SNAPSHOT_DISK),
                          ("DiskBookSuperblock", ondisk.DISKBOOK_SUPERBLOCK),
                          ("DiskBookEntry", ondisk.DISKBOOK_ENTRY),
                          ("UseChainLink", ondisk.TUSE_LINK)):
        c_fields = {f: (o, s) for f, o, s in structs.get(cname, [])}
        for field, off, size in fields.Offsets():
            Eq("%s.%s" % (cname, field), (off, size), c_fields.get(field))
    ledger = {f: o for f, o, _s in structs.get("VolumeLedger", [])}
    Eq("VOLUME_LEDGER_REQUIRED_BYTES", ondisk.LEDGER_REQUIRED_BYTES, ledger.get("use_context_offset"))
    for field, off, _size in ondisk.USE_CHAIN.Offsets():
        Eq("VolumeLedger.use_chain_%s" % field, ondisk.LEDGER_USE_CHAIN_AT + off,
           ledger.get("use_chain_%s" % field))


# ---------------------------------------------------------------------------
# X2: the owners of the data run, held against the header and heard
# ---------------------------------------------------------------------------


def OwnersCheck(src, heard):
    """What is wrong with ground_owners.py: not what tools/gen_ground_owners.py
    writes out of src/include/ground_owner.h today, or an owner the header
    names that no case's census heard claim a block."""
    gen = os.path.join(os.path.dirname(src), "tools", "gen_ground_owners.py")
    spec = importlib.util.spec_from_file_location("gen_ground_owners", gen)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    wrong = []
    with open(module.OUT) as fh:
        if fh.read() != module.Text(module.Owners()):
            wrong.append("%s is stale against %s — run tools/gen_ground_owners.py"
                         % (module.OUT, module.HEADER))
    unheard = [name for name, _alone in GROUND_OWNERS if name not in heard]
    if unheard:
        wrong.append("no case's census heard %s claim a block" % ", ".join(unheard))
    return wrong


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
