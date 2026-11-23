#!/usr/bin/env python3
"""crossings.py — the crossings a BoxOS medium keeps in its ESP's
EFI/BoxOS/crossings (src/include/boxos_crossings.h, boxos_esp.h): every start
Gangway handed a kernel over, and whether it landed.

  crossings.py file OUT     the file the crossings are kept in — its head and
                            every slot empty — into OUT, for the ESP the tool
                            that makes a medium lays
  crossings.py read IMAGE   the head, and every crossing that checks out,
                            newest first, a line each, the file found in the
                            image's ESP by build/fat; exit 1 when it carries
                            none, or its head does not hold

--block N: the block the image's medium is addressed in (4096 for a 4Kn
medium, 512 by default) — its partition table and its ESP count in it.

The numbers of the layout are read out of boxos_crossings.h and boxos_esp.h,
and the two records are held to the sizes it asserts, so nothing here is a
second copy of what the loader and the kernel read.
"""

import os
import re
import struct
import sys
import subprocess
import zlib

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
HEADER = os.path.join(ROOT, "src", "include", "boxos_crossings.h")
ESP_HEADER = os.path.join(ROOT, "src", "include", "boxos_esp.h")
FAT = os.path.join(ROOT, "build", "fat")
ESP_TYPE = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")   # c12a7328-f81f-11d2-ba4b-00a0c93ec93b, as on the medium

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


def esp_path():
    return re.search(r'#define\s+ESP_CROSSINGS\s+"([^"]+)"', open(ESP_HEADER).read()).group(1)


def esp_first(medium, block):
    """The first block of the image's ESP — the MBR's entry of type 0xEF, or
    the GPT's of the ESP's type — or None."""
    medium.seek(0)
    mbr = medium.read(512)
    if len(mbr) < 512 or mbr[510:512] != b"\x55\xaa":
        return None
    kinds = [(mbr[446 + 16 * i + 4], struct.unpack_from("<I", mbr, 446 + 16 * i + 8)[0]) for i in range(4)]
    if not any(k == 0xEE for k, _ in kinds):
        return next((start for k, start in kinds if k == 0xEF), None)
    medium.seek(block)
    head = medium.read(92)
    if head[:8] != b"EFI PART":
        return None
    entries, count, size = struct.unpack_from("<QII", head, 72)
    medium.seek(entries * block)
    array = medium.read(count * size)
    for i in range(count):
        e = array[i * size:(i + 1) * size]
        if e[:16] == ESP_TYPE:
            return struct.unpack_from("<Q", e, 32)[0]
    return None


def file_bytes(n):
    head = HEAD.pack(n["CROSSINGS_HEAD_MAGIC"], HEAD.size, 0, n["CROSSINGS_SLOTS"],
                     n["CROSSINGS_SLOT_SECTORS"], n["CROSSINGS_FIRST_SLOT"])
    crc = zlib.crc32(head) & 0xFFFFFFFF
    head = head[:12] + struct.pack("<I", crc) + head[16:]
    return head.ljust(n["CROSSINGS_FILE_BYTES"], b"\0")


def read(path, n, block):
    with open(path, "rb") as medium:
        first = esp_first(medium, block)
        if first is None:
            print(f"crossings: {path}: it carries no ESP")
            return 1
        run = subprocess.run([FAT, "--sector", str(block), "--at", str(first), path, "run", esp_path()],
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE, universal_newlines=True)
        if run.returncode != 0:
            print(f"crossings: {path}: its ESP carries no {esp_path()} in one run ({run.stderr.strip()})")
            return 1
        base = int(run.stdout.split()[0]) * (block // 512)
        medium.seek(base * 512)
        front = medium.read(n["CROSSINGS_FILE_BYTES"])
    head = front[:HEAD.size]
    if not sound(head, HEAD, n["CROSSINGS_HEAD_MAGIC"]):
        print(f"crossings: {path}: its {esp_path()} carries no head")
        return 1
    _, _, _, slots, slot_sectors, first_slot = HEAD.unpack(head)
    print(f"crossings: head at sector {base}, {slots} slots of {slot_sectors} sectors "
          f"from sector {base + first_slot}")
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
    if len(argv) != 3 or argv[1] not in ("file", "read") or block % 512 or not 512 <= block <= 4096:
        sys.exit(__doc__)
    n = layout()
    if argv[1] == "file":
        with open(argv[2], "wb") as out:
            out.write(file_bytes(n))
        return 0
    return read(argv[2], n, block)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
