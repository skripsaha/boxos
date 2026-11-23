#!/usr/bin/env python3
"""crossings.py — the crossings kept in the front of a BoxOS medium
(src/include/boxos_crossings.h): every start Gangway handed a kernel over, and
whether it landed.

  crossings.py lay IMAGE    lay the head the crossings are kept under, every
                            slot empty — what the tool that makes a medium
                            does once its partition table is written
  crossings.py read IMAGE   the head, and every crossing that checks out,
                            newest first, a line each; exit 1 when the front
                            carries no head

--block N: the block the image's medium is addressed in (4096 for a 4Kn
medium, 512 by default) — its partition table counts in it.

The numbers of the layout are read out of boxos_crossings.h, and the two
records are held to the sizes it asserts, so nothing here is a second copy of
what the loader and the kernel read.
"""

import os
import re
import struct
import sys
import zlib

HEADER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "src", "include", "boxos_crossings.h")

HEAD = struct.Struct("<8sIIIIQ")
CROSSING = struct.Struct("<8sIIQ16sH10B7IB15s")
FIELDS = ("magic", "size", "crc", "number", "volume", "year", "month", "day", "hour", "minute", "second",
          "clock", "kernel", "panel", "startup", "keep_log", "asked", "cores", "screen_width", "screen_height",
          "misses", "landed", "landed_ms", "scene", "reserved")


def layout():
    """Every #define of boxos_crossings.h, its value worked out, and the
    sizes it asserts of its two records."""
    text = open(HEADER).read()
    names = {}
    for name, value in re.findall(r"^#define\s+(CROSSINGS?_\w+)\s+(.+?)\s*(?:/\*.*)?$", text, re.M):
        if value.startswith('"'):
            names[name] = value.strip('"').encode()
            continue
        expr = re.sub(r"\b(\d+)u\b", r"\1", value)
        names[name] = eval(expr, {"__builtins__": {}}, dict(names))
    sizes = dict(re.findall(r"sizeof\((\w+)\)\s*==\s*(\d+)", text))
    if int(sizes["CrossingsHead"]) != HEAD.size or int(sizes["Crossing"]) != CROSSING.size:
        sys.exit("crossings: boxos_crossings.h asserts records of other sizes than this tool reads")
    return names


def sound(raw, packer, magic):
    if raw[:8] != magic or struct.unpack_from("<I", raw, 8)[0] != packer.size:
        return False
    crc = struct.unpack_from("<I", raw, 12)[0]
    return zlib.crc32(raw[:12] + b"\0\0\0\0" + raw[16:packer.size]) & 0xFFFFFFFF == crc


def front_free(medium, end, block):
    """The MBR names every partition at `end` or past it, and is no GPT's —
    its entries in the medium's blocks of `block` bytes."""
    medium.seek(0)
    mbr = medium.read(512)
    if len(mbr) < 512 or mbr[510:512] != b"\x55\xaa":
        return "it carries no MBR"
    for i in range(4):
        entry = mbr[446 + 16 * i: 462 + 16 * i]
        kind, start = entry[4], struct.unpack_from("<I", entry, 8)[0] * (block // 512)
        if kind == 0xEE:
            return "its partition table is a GPT's"
        if kind and start < end:
            return f"partition {i + 1} begins at sector {start}, inside the front"
    return None


def lay(path, n, block):
    with open(path, "r+b") as medium:
        why = front_free(medium, n["CROSSINGS_FRONT_END"], block)
        if why:
            sys.exit(f"crossings: {path}: no crossings laid — {why}")
        head = HEAD.pack(n["CROSSINGS_HEAD_MAGIC"], HEAD.size, 0, n["CROSSINGS_SLOTS"],
                         n["CROSSINGS_SLOT_SECTORS"], n["CROSSINGS_FIRST_SLOT"])
        crc = zlib.crc32(head) & 0xFFFFFFFF
        head = head[:12] + struct.pack("<I", crc) + head[16:]
        front = head.ljust(n["CROSSINGS_SLOT_BYTES"], b"\0") + bytes(n["CROSSINGS_SLOTS"] * n["CROSSINGS_SLOT_BYTES"])
        medium.seek(n["CROSSINGS_HEAD_SECTOR"] * 512)
        medium.write(front)
    print(f"crossings: laid at sector {n['CROSSINGS_HEAD_SECTOR']}, {n['CROSSINGS_SLOTS']} slots from sector "
          f"{n['CROSSINGS_FIRST_SLOT']}")


def read(path, n):
    with open(path, "rb") as medium:
        medium.seek(n["CROSSINGS_HEAD_SECTOR"] * 512)
        front = medium.read((n["CROSSINGS_END_SECTOR"] - n["CROSSINGS_HEAD_SECTOR"]) * 512)
    head = front[:HEAD.size]
    if not sound(head, HEAD, n["CROSSINGS_HEAD_MAGIC"]):
        print(f"crossings: {path}: its front carries no head")
        return 1
    _, _, _, slots, slot_sectors, first = HEAD.unpack(head)
    print(f"crossings: head at sector {n['CROSSINGS_HEAD_SECTOR']}, {slots} slots of {slot_sectors} sectors "
          f"from sector {first}")
    kept = []
    for s in range(n["CROSSINGS_SLOTS"]):
        raw = front[(1 + s) * n["CROSSINGS_SLOT_BYTES"]:][:CROSSING.size]
        if sound(raw, CROSSING, n["CROSSING_MAGIC"]):
            kept.append((s, dict(zip(FIELDS, CROSSING.unpack(raw)))))
    kept.sort(key=lambda k: -k[1]["number"])
    panels = {n["CROSSING_PANEL_NONE"]: "none", n["CROSSING_PANEL_KEY"]: "key", n["CROSSING_PANEL_MISSES"]: "misses"}
    for slot, c in kept:
        when = (f"{c['year']:04}-{c['month']:02}-{c['day']:02} {c['hour']:02}:{c['minute']:02}:{c['second']:02}"
                if c["clock"] else "no-clock")
        landed = f"landed ms={c['landed_ms']}" if c["landed"] == n["CROSSING_LANDED"] else "not-landed"
        kernel = "held" if c["kernel"] == n["CROSSING_KERNEL_HELD"] else "unchecked"
        print(f"crossing {c['number']} slot {slot} {when} {landed} misses={c['misses']} "
              f"panel={panels.get(c['panel'], c['panel'])} kernel={kernel} "
              f"screen={c['screen_width']}x{c['screen_height']} asked={c['asked']}")
    print(f"crossings: {len(kept)} kept")
    return 0


def main(argv):
    block = 512
    if "--block" in argv:
        at = argv.index("--block")
        block = int(argv[at + 1], 0)
        argv = argv[:at] + argv[at + 2:]
    if len(argv) != 3 or argv[1] not in ("lay", "read") or block % 512 or not 512 <= block <= 4096:
        sys.exit(__doc__)
    n = layout()
    if argv[1] == "lay":
        lay(argv[2], n, block)
        return 0
    return read(argv[2], n)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
