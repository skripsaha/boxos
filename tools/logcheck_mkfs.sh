# logcheck_mkfs — the mkfs scenario of tools/logcheck.sh: what create_tagfs
# lays down, held on the host to volume-read — the format's second reader and
# the binder it keeps — before any machine is asked. No boot.
#
# SOURCED, NEVER RUN — by logcheck.sh, which dispatches `mkfs` to run_mkfs
# here; ok, bad, chk, harness, build, build_with_key, buildkey_restore and
# SCRATCH are stand.sh's. Its own file for the reason logcheck_framerefuse.sh
# has one: logcheck.sh is twelve thousand lines.
#
# ‼ THE WRITER IS HELD TO THE READER, NOT TO ITSELF. `make VOLUME_FORMAT=v2`
# runs volume-read's census over the image it made, and that census only asks
# whether the book holds together. Whether it is the book a binder lays of the
# same files — node for node, byte for byte, where §16.3 п.5 says it lies — is
# asked by self-test's B0b and B0d, which on an image mkfs bound fork every
# landing from mkfs's own book; on the v1 image a plain `make` lays, the book
# under them is laid by volume-read's binder itself and they compare it with
# itself. So the self-test is run here over the image `make VOLUME_FORMAT=v2`
# lays — all its files, for the landings that fork a tree need items enough in
# it (on a volume of a few files two of them answer NOT so) — and every file on
# it is held to the same file on the v1 image of the same tree, and its ground
# to the formula --ground --v2 answers by (§16.2 п.23), the working room a v1
# volume is given included.
#
# Then what no image of the build can show, each on a sparse ground of its own:
# a ground past 16 TiB, which --v2 takes and format v1 refuses; a ground the
# book does not fit, refused with not a byte of it written; and the cargo's
# widths — a vocabulary past 65535 tags, a file of hundreds of tags in a list
# longer than four kilobytes, both carried whole by --v2, each file naming its
# own tags; a vocabulary past format v1's last tag id, a record past its pool
# block, a tag wider than the kernel holds — each refused before the first
# write, in a sentence. Last, `make` lays the run's format again: the format's
# marker.
#
# ‼ THE BUILD'S IMAGE PACKS NO TREE PAST ONE LEAF. Its FILES and its TAGS fit
# a Sheet each, and every DIGESTS element fills one alone, so where a node is
# cut is never asked of it: a binder that packed a few bytes short would pass
# B0b there. The wide vocabulary's book is a hundred and seventy-odd nodes —
# FILES and TAGS many leaves wide under inner nodes, one of them filled to the
# byte — and it is bound again and held where it lies the same way.

MKFS_CLEAN="check: consistent — 0 damage, 0 leak, 0 note"

# mkfs_py LEG ARGS... — the host's half of each leg: it makes what the leg
# needs, runs create_tagfs and reads back what it laid, and prints one line,
# MKFS k=v ..., for the shell to judge. Legs:
#   ground IMG SECTORS   a sparse image: a protective MBR and a GPT whose one
#                        entry is a BoxOS partition of SECTORS sectors from
#                        sector 2048 (the type GUID ground.c reads)
#   files V1 V2          every file of V1 (format v1) against V2 (format v2),
#                        by fid: name and bytes
#   formula IMG          the ground IMG stands on against §16.2 п.23 over its
#                        own book
#   wide|v1ids|v1record|keywide WORK   the cargo's widths (above)
#   roles WORK           a format-v2 volume of the kernel, a system box, a plain
#                        box and a text a person tagged system, boot, god and
#                        kernel: each file's role in its record is what its
#                        box says, and the kernel is the file --kernel names
mkfs_py() {
    python3 - "$@" <<'PY'
import binascii, hashlib, os, struct, subprocess, sys, uuid
sys.dont_write_bytecode = True
sys.path.insert(0, "tools")
from volumeread import binder, medium, ondisk
from volumeread.bookcensus import BookCensus
from volumeread.census import GroundCensus

TOOL, START = "tools/create_tagfs", 2048


def Ground(img, sectors):
    last = START + sectors - 1
    total = last + 1 + 33
    entry = (ondisk.BOXOS_TYPE_GUID + uuid.uuid4().bytes_le + struct.pack("<QQQ", START, last, 0) +
             "BoxOS".encode("utf-16-le").ljust(72, b"\0"))
    array = entry.ljust(128 * 128, b"\0")
    head = bytearray(struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0, 1, total - 1, 34,
                                 total - 34, uuid.uuid4().bytes_le, 2, 128, 128, binascii.crc32(array)))
    struct.pack_into("<I", head, 16, binascii.crc32(bytes(head)))
    mbr = bytearray(512)
    mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF, 0xFF, 0xFF, 1, min(total - 1, 0xFFFFFFFF))
    mbr[510:512] = b"\x55\xaa"
    with open(img, "wb") as f:
        f.truncate(total * 512)
        f.write(mbr + bytes(head).ljust(512, b"\0") + array)


def Sha(img):
    h = hashlib.sha256()
    with open(img, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def Mkfs(*args):
    r = subprocess.run([TOOL] + list(args), capture_output=True, text=True, errors="replace")
    return r.returncode, (r.stdout + r.stderr).strip().splitlines()


def Files(work, prefix, tag_lists):
    """A file of one block of its own bytes for each list of tags, and the
    argv pairs that give them."""
    args = []
    for i, tags in enumerate(tag_lists):
        path = os.path.join(work, "%s%04u.bin" % (prefix, i))
        with open(path, "wb") as f:
            f.write(("%s %u" % (prefix, i)).encode().ljust(4096, b"."))
        args += [path, ",".join(tags)]
    return args


def Refused(work, name, mode, args, says):
    """create_tagfs MODE over a fresh ground: refused, in so many words, and
    not a byte of the image written."""
    img = os.path.join(work, name + ".img")
    Ground(img, 131072)
    before = Sha(img)
    rc, out = Mkfs(mode, img, *args)
    said = [l for l in out if says in l]
    print("MKFS %s refused=%u untouched=%u said=%s" % (name, rc != 0 and bool(said), Sha(img) == before,
                                                      (said or out or ["nothing"])[-1][:150]))
    os.remove(img)


def Laid(img):
    """The image's book against volume-read's binder: every node and slot 0
    bound again from what was read, byte for byte (binder.Circle), and the
    Tally-Quires, the ribbon and its next_quire where a binder lays them past
    the files (§16.3 п.5) — what self-test's B0b and B0d ask, on a Tally of
    trees many leaves wide."""
    c = BookCensus(medium.Medium(img))
    m, p = c.mount, binder.ReadPlan(c.mount.tally)
    end = max([c.g.data_block] + [r.start + r.count for fs in m.files.values() for r in fs.runs if not r.hole])
    fresh = binder.Bind(m.tally.content, c.g, after=end)
    wrong = binder.Circle(medium.Medium(img), m)
    laid = (fresh.a, fresh.b, tuple(fresh.ribbon), tuple(fresh.next)) == (p.a, p.b, tuple(p.ribbon), tuple(p.next))
    return c, "nodes=%u circle=%s laid=%u" % (len(m.tally.Nodes()), wrong[0].replace(" ", "_") if wrong else "same",
                                              laid)


def Wide(work):
    """Past 65535 tags in all and one file of 700 in a list of 4.9 KB: every
    file names its own tags, by their text, and the book — its FILES and TAGS
    trees many leaves wide under inner nodes — is the one a binder lays."""
    lists = [["t%x" % (i * 252 + j) for j in range(252)] for i in range(260)]
    lists.append(["w%04u" % j for j in range(700)])
    args = Files(work, "wide", lists)
    rc, out = Mkfs("--ground", "--v2", *args)
    if rc != 0:
        print("MKFS wide made=0 why=%s" % out[-1:])
        return
    img = os.path.join(work, "wide.img")
    Ground(img, int(out[-1]))
    rc, out = Mkfs("--v2", img, *args)
    if rc != 0:
        print("MKFS wide made=0 why=%s" % out[-1:])
        return
    c, laid = Laid(img)
    m = c.mount
    want = {"wide%04u.bin" % i: {b"wide%04u" % i} | {t.encode() for t in tags} for i, tags in enumerate(lists)}
    wrong = []
    for fs in m.files.values():
        have = {m.tags[t].Text() for t in fs.tags}
        if have != want.get(fs.name.decode(), None):
            wrong.append("%s names %u tag(s), %u not its own" % (fs.name.decode(), len(have),
                                                               len(have - want.get(fs.name.decode(), set()))))
    print("MKFS wide made=1 files=%u tags=%u clean=%u glass=%u named=%s %s" % (
        len(m.files), len(m.tags), not c.findings, bool(m.glass), wrong[0] if wrong else "own", laid))
    os.remove(img)


def Roles(work):
    """No tag gives a file a role: the volume keeps the kernel in the file --kernel names, a
    box in the role its own part `role` says, and every other file ordinary — the text
    wears the words system, boot, god and kernel and is kept as an ordinary file."""
    img, text = os.path.join(work, "roles.img"), os.path.join(work, "words.txt")
    with open(text, "w") as f:
        f.write("a text a person tagged with the words of the system's roles\n")
    Ground(img, 524288)
    kept = {"kernel.bin": ondisk.FILE_ROLE_MASK, "hub.box": ondisk.FILE_ROLE_SYSTEM, "clock.box": 0, "words.txt": 0}
    rc, out = Mkfs("--v2", "--kernel", "build/kernel.bin", img, "build/kernel.bin", "", "build/box/hub.box", "",
                   "build/box/clock.box", "", text, "system,boot,god,kernel,app,utility")
    roles = {}
    if rc == 0:
        mount = BookCensus(medium.Medium(img)).mount
        roles = {fs.name.decode(): fs.flags & ondisk.FILE_ROLE_MASK for fs in mount.files.values()}
    print("MKFS roles made=%u kept=%u %s" % (rc == 0, roles == kept, " ".join("%s=%u" % r for r in sorted(roles.items()))))
    os.remove(img)


def Leg(leg, a):
    if leg == "roles":
        Roles(a[0])
    elif leg == "ground":
        Ground(a[0], int(a[1]))
    elif leg == "files":
        one, two = GroundCensus(medium.Medium(a[0])), BookCensus(medium.Medium(a[1]))
        have = {fid: (fs.name.decode(), two.Sha256(fid)) for fid, fs in two.mount.files.items()}
        want = {fid: (e["record"].name if isinstance(e["record"].name, str) else e["record"].name.decode(),
                      one.Sha256(e["record"])) for fid, e in one.files.items() if e["state"] == "live"}
        differ = sorted(fid for fid in set(have) | set(want) if have.get(fid) != want.get(fid))
        print("MKFS files=%u same=%u differ=%s" % (len(want), len(want) - len(differ), differ[:5] or "none"))
    elif leg == "formula":
        c = BookCensus(medium.Medium(a[0]))
        p, g = binder.ReadPlan(c.mount.tally), c.g
        k = (p.b - p.a) // g.grain
        room = 64 * 1024 * 1024 // ondisk.BLOCK          # create_tagfs.h GROUND_WORKING_ROOM_BLOCKS
        end = p.next[0] + p.next[1] + (1 + 2 * k) * g.grain + room + 1
        print("MKFS formula=%u ground=%u k=%u" % (-(-end // g.grain) * g.grain * ondisk.SECTORS_PER_BLOCK,
                                                  g.stand.deed["sectors"], k))
    elif leg == "wide":
        Wide(a[0])
    elif leg == "v1ids":
        lists = [["v%x" % (i * 252 + j) for j in range(252)] for i in range(130)]
        args = Files(a[0], "ids", lists)
        Refused(a[0], "v1ids", "--v1", args, "its format carries none past 32766")
        rc, out = Mkfs("--ground", "--v1", *args)
        said = [l for l in out if "its format carries none past 32766" in l]
        print("MKFS v1ground refused=%u said=%s" % (rc != 0 and bool(said), (said or out or ["nothing"])[-1][:150]))
    elif leg == "v1record":
        Refused(a[0], "v1record", "--v1", Files(a[0], "rec", [["r%u" % j for j in range(2100)]]),
                "format v1 keeps a file's record in one pool block")
    elif leg == "keywide":
        for mode in ("--v1", "--v2"):
            Refused(a[0], "keywide" + (mode if mode == "--v2" else ""), mode, Files(a[0], "key", [["k" * 256]]),
                    "the kernel holds a tag's key in 255 bytes")
            Refused(a[0], "textwide" + (mode if mode == "--v2" else ""), mode, Files(a[0], "text", [["k:" + "v" * 4074]]),
                    "the kernel holds a tag's key in 255 bytes")


Leg(sys.argv[1], sys.argv[2:])
PY
}

# mkfs_image WORK — `make VOLUME_FORMAT=v2`, and the image it lays held to the
# v1 image of the same tree (WORK/v1.img), to §16.2 п.23 and to self-test.
mkfs_image() {
    local work=$1 said line
    if ! build_with_key VOLUME_FORMAT=v2; then
        bad "mkfs: make VOLUME_FORMAT=v2 does not build: $(grep -aE 'ERROR|check:|error' "$SCRATCH/build.log" | tail -1)"
        return
    fi
    said=$(grep -a '^  check: ' "$SCRATCH/build.log" | tail -1)
    [ "$said" = "  $MKFS_CLEAN" ]
    chk $? "mkfs: make VOLUME_FORMAT=v2 builds, its census clean in the build's own words (${said# })"
    line=$(python3 tools/volume-read.py build/boxos.img check | grep -a '^check: the kernel in this tree would NOT mount')
    [ -z "$line" ] || grep -aqF "  WARNING: ${line#check: }" "$SCRATCH/build.log"
    chk $? "mkfs: and the build says what the census says of this tree's kernel (${line:-it would mount it})"
    python3 tools/volume-read.py build/boxos.img deed | grep -q '^format      : v2'
    chk $? "mkfs: build/boxos.img is format v2"
    said=$(mkfs_py files "$work/v1.img" build/boxos.img)
    echo "$said" | grep -qE '^MKFS files=([0-9]+) same=\1 differ=none$'
    chk $? "mkfs: every file on it is the file the v1 image of this tree carries, name and bytes (${said#MKFS })"
    said=$(mkfs_py formula build/boxos.img)
    echo "$said" | grep -qE '^MKFS formula=([0-9]+) ground=\1 '
    chk $? "mkfs: its ground is what --ground --v2 answers by §16.2 п.23 over the book it lays (${said#MKFS })"
    python3 tools/volume-read.py build/boxos.img self-test >"$work/selftest.out" 2>&1
    chk $? "mkfs: self-test over mkfs's own book — B0b binds it again byte for byte, B0d holds it where it lies, every landing forks from it ($(grep -E '^self-test: ' "$work/selftest.out" | tail -1))"
    grep -E 'NOT so|missing|unexpected' "$work/selftest.out" | head -5 | sed 's/^/    /'
}

# mkfs_grounds WORK — a ground past 16 TiB, and one the book does not fit.
mkfs_grounds() {
    local work=$1 img=$1/big.img kb sb blocks before rc
    mkfs_py ground "$img" $((17 * 2147483648))
    tools/create_tagfs --v2 --kernel build/kernel.bin "$img" build/box/hub.box "" build/kernel.bin "" \
        >"$work/big.log" 2>&1
    chk $? "mkfs: --v2 lays a volume on a ground of 17 TiB ($(grep -a 'Ground' "$work/big.log" | head -1 | sed 's/^ *//'))"
    [ "$(python3 tools/volume-read.py "$img" check | tail -1)" = "$MKFS_CLEAN" ]
    chk $? "mkfs: and its census reads it clean"
    ! tools/create_tagfs --v1 "$img" build/box/hub.box "" >"$work/big.v1.log" 2>&1 \
        && grep -aq '16 TiB' "$work/big.v1.log"
    chk $? "mkfs: format v1 refuses the same ground ($(grep -a '16 TiB' "$work/big.v1.log" | head -1 | cut -c1-90))"
    rm -f "$img"

    img=$work/small.img
    kb=$(( ($(wc -c <build/kernel.bin) + 4095) / 4096 ))
    sb=$(( ($(wc -c <build/box/hub.box) + 4095) / 4096 ))
    blocks=$(( ( (1024 + kb + sb + 255) / 256 ) * 256 + 256 + 1 ))
    mkfs_py ground "$img" $((blocks * 8))
    before=$(shasum -a 256 <"$img")
    tools/create_tagfs --v2 --kernel build/kernel.bin "$img" build/box/hub.box "" build/kernel.bin "" \
        >"$work/small.log" 2>&1
    rc=$?
    [ $rc -ne 0 ] && grep -aq "does not fit" "$work/small.log"
    chk $? "mkfs: a ground of $blocks blocks is refused: $(grep -a 'does not fit' "$work/small.log" | head -1 | cut -c1-120)"
    [ "$before" = "$(shasum -a 256 <"$img")" ]
    chk $? "mkfs: and not one byte of it is written"
    rm -f "$img"
}

# mkfs_tags WORK — the cargo's widths: carried whole where the format has
# none, refused before the first write where a format or the kernel has one.
mkfs_tags() {
    local work=$1 said
    said=$(mkfs_py wide "$work")
    echo "$said" | grep -qE '^MKFS wide made=1 files=261 tags=[0-9]+ clean=1 glass=0 named=own ' \
        && [ "$(echo "$said" | sed -nE 's/.* tags=([0-9]+) .*/\1/p')" -gt 65535 ]
    chk $? "mkfs: --v2 carries a vocabulary past 65535 tags and a file of 700 in 4.9 KB of list, each file naming its own (${said#MKFS })"
    echo "$said" | grep -qE ' circle=same laid=1$'
    chk $? "mkfs: and its book, trees many leaves wide, is the one volume-read's binder lays, byte for byte and where it lies ($(echo "$said" | sed -nE 's/.* (nodes=.*)/\1/p'))"
    said=$(mkfs_py v1ids "$work")
    echo "$said" | grep -q '^MKFS v1ids refused=1 untouched=1 '
    chk $? "mkfs: format v1 refuses a vocabulary past tag id 32766 before a byte — $(echo "$said" | grep '^MKFS v1ids' | sed 's/^MKFS //')"
    echo "$said" | grep -q '^MKFS v1ground refused=1 '
    chk $? "mkfs: and --ground refuses the same list — $(echo "$said" | grep '^MKFS v1ground' | sed 's/^MKFS //')"
    said=$(mkfs_py v1record "$work")
    echo "$said" | grep -q '^MKFS v1record refused=1 untouched=1 '
    chk $? "mkfs: format v1 refuses a file of 2100 tags, whose record no pool block holds, before a byte — ${said#MKFS }"
    said=$(mkfs_py keywide "$work")
    [ "$(echo "$said" | grep -c ' refused=1 untouched=1 ')" = 4 ]
    chk $? "mkfs: a key of 256 bytes, and a tag of 4075, refused by both formats before a byte — $(echo "$said" | sed 's/^MKFS //; s/ said=.*//' | paste -sd ';' -)"
    echo "$said" | grep -v ' refused=1 untouched=1 ' | sed 's/^/    /'
}

# mkfs_roles WORK — what the volume keeps a file as comes from the kernel the
# maker is told and from the box, and from no tag.
mkfs_roles() {
    local said
    said=$(mkfs_py roles "$1")
    echo "$said" | grep -qE '^MKFS roles made=1 kept=1 '
    chk $? "mkfs: the kernel is the file --kernel names, a box says its own role, and a text tagged system, boot, god and kernel stays ordinary (${said#MKFS roles })"
}

run_mkfs() {
    echo "== mkfs: what create_tagfs lays down, held on the host to the format's second reader =="
    local work="$SCRATCH/mkfs" format
    format=$(stand_format)
    # The v1 image is built as v1 by name: the format the run asks — the gate's
    # VOLUME_FORMAT, or the Makefile's when nobody names one — is the one the
    # tree is put back in, and neither is format v1 for certain.
    STAND_FORMAT=v1 build
    rm -rf "$work"
    mkdir -p "$work"
    python3 tools/volume-read.py build/boxos.img deed | grep -q '^format      : v1' \
        && cp build/boxos.img "$work/v1.img" || { harness "mkfs: could not keep a v1 image"; return; }
    mkfs_image "$work"
    mkfs_grounds "$work"
    mkfs_tags "$work"
    mkfs_roles "$work"
    buildkey_restore
    # The v1 build above was plain: the tree is put back in the run's format
    # whether or not a keyed build came after it. The deed names the format
    # and then its stamps — `format      : v2 (v1 = LAYOUT, v2 = FRAME)`,
    # volume-read.py — so the word is matched with the space after it.
    python3 tools/volume-read.py build/boxos.img deed | grep -q "^format      : $format " ||
        STAND_FORMAT=$format build
    python3 tools/volume-read.py build/boxos.img deed | grep -q "^format      : $format "
    chk $? "mkfs: and \`make\` after it lays format $format again, the format the run asked — the image carries its format's marker"
    rm -rf "$work"
}
