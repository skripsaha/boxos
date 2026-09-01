#!/bin/sh
# rgbcheck — the console-epic S1 colour oracle, both firmwares:
#
#   UEFI/GOP: a #RRGGBB named by userspace reaches the framebuffer
#   bit-exactly. Exact equality, no tolerance — the render path is integer
#   end to end, so a single off-by-one is a real defect, not noise.
#
#   BIOS/VGA text: the same probe quantises to the 16-colour attribute by
#   DOMINANT HUE (dark navy reads BLUE, dark purple reads MAGENTA — never
#   swallowed by black or gray). The VRAM attribute bytes are read back
#   over the QEMU monitor and compared against the defined projection —
#   this is the "do GOP and VGA agree" question as a standing check: same
#   probe, exact on GOP, hue-faithful on VGA.
#
# Both halves boot STRICT and run the rgbtest utility — it verifies the
# SET/GET wire round-trip in-band, then paints known-colour cells: row 1
# through the printf → display-daemon path (%color/%bgcolor, background
# probes), rows 7-8 through direct PUTCHAR ops (absolute coordinates,
# immune to any concurrent cursor movement).
#
# Cell geometry: 8x16 font on GOP, cell (row R, col C) → pixel (C*8, R*16);
# each probed cell is sampled at its centre. Row 7 carries 0xDB full-block
# glyphs (every pixel is foreground); rows 1 and 8 carry spaces (every
# pixel is background).

set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT" || exit 2

# Absolute: the QEMU monitor resolves screendump paths against ITS cwd.
PPM="$ROOT/build/rgb_probe.ppm"

fail() { printf 'rgbcheck: \033[31mFAIL\033[0m %s\n' "$1"; make run-stop >/dev/null 2>&1; exit 1; }

# grep -a everywhere: the probe's serial mirror carries raw 0xDB block
# bytes, which flips BSD grep into binary mode and silently stops text
# matching past them without -a.
wait_serial() {  # $1 = pattern, $2 = seconds budget
    budget=$2
    start=$(date +%s)
    while :; do
        grep -aq "$1" build/serial.log 2>/dev/null && return 0
        now=$(date +%s)
        [ $((now - start)) -ge "$budget" ] && return 1
        sleep 2
    done
}

run_rgbtest() {  # $1 = firmware label
    wait_serial "BoxOS Shell" 120 || fail "$1: shell banner never appeared on serial"
    sleep 3
    ./tools/qemu-input.sh type "rgbtest" >/dev/null 2>&1
    sleep 1
    ./tools/qemu-input.sh key ret >/dev/null 2>&1
    wait_serial "\[RGBTEST\] \(ALL PASS\|FAILED\)" 120 || fail "$1: rgbtest never finished"
    grep -aq "\[RGBTEST\] FAILED" build/serial.log && fail "$1: rgbtest reported failures (see build/serial.log)"
    # The daemon renders the printf-path rows asynchronously; the ALL PASS
    # line travels that same pipe, so once it is on the serial the probe
    # rows printed before it are rendered too. One settle beat for the blit.
    sleep 2
}

# ───── UEFI/GOP half: bit-exact pixels ─────

make run-stop >/dev/null 2>&1
UEFI=on STRICT=on make run-bg >/dev/null 2>&1 || fail "make run-bg UEFI=on did not start"
run_rgbtest "UEFI"

./tools/qemu-input.sh shot "$PPM" >/dev/null 2>&1 || fail "screendump failed"

python3 - "$PPM" <<'GOPEOF'
import sys

path = sys.argv[1]
data = open(path, 'rb').read()

# P6 header: magic, width, height, maxval — whitespace-separated, # comments.
pos = 0
fields = []
while len(fields) < 4:
    while pos < len(data) and data[pos:pos+1].isspace():
        pos += 1
    if data[pos:pos+1] == b'#':
        while pos < len(data) and data[pos] != 0x0A:
            pos += 1
        continue
    start = pos
    while pos < len(data) and not data[pos:pos+1].isspace():
        pos += 1
    fields.append(data[start:pos])
pos += 1  # single whitespace after maxval
magic, w, h, maxval = fields[0], int(fields[1]), int(fields[2]), int(fields[3])
if magic != b'P6' or maxval != 255:
    print(f'rgbcheck: FAIL unsupported PPM ({magic!r} maxval={maxval})')
    sys.exit(1)
px = data[pos:]

def pixel(x, y):
    o = (y * w + x) * 3
    return (px[o], px[o+1], px[o+2])

# Mirrors rgbtest.c exactly.
probes = [
    # (row, col, want_rgb, label)
    (1,  1, (0x40, 0x20, 0x60), 'printf-path bg #402060'),
    (1,  9, (0x12, 0x34, 0x56), 'printf-path %bgcolor #123456'),
    (7,  1, (0xFF, 0x00, 0x00), 'direct fg #FF0000'),
    (7,  5, (0x00, 0xFF, 0x00), 'direct fg #00FF00'),
    (7,  9, (0x00, 0x00, 0xFF), 'direct fg #0000FF'),
    (7, 13, (0x10, 0x20, 0x30), 'direct fg #102030'),
    (7, 17, (0xFF, 0xB0, 0x40), 'direct fg #FFB040 (amber)'),
    (7, 21, (0xFF, 0xFF, 0xFF), 'direct fg #FFFFFF'),
    (8,  1, (0x40, 0x20, 0x60), 'direct bg #402060'),
    (8,  5, (0x12, 0x34, 0x56), 'direct bg #123456'),
]

bad = 0
for row, col, want, label in probes:
    x, y = col * 8 + 4, row * 16 + 8
    if x >= w or y >= h:
        print(f'rgbcheck: FAIL {label}: sample ({x},{y}) outside {w}x{h}')
        bad += 1
        continue
    got = pixel(x, y)
    if got == want:
        print(f'rgbcheck: PASS GOP {label} at cell ({row},{col})')
    else:
        print(f'rgbcheck: FAIL GOP {label} at cell ({row},{col}): '
              f'got #{got[0]:02X}{got[1]:02X}{got[2]:02X}')
        bad += 1

sys.exit(1 if bad else 0)
GOPEOF
rc_gop=$?

make run-stop >/dev/null 2>&1
[ $rc_gop -eq 0 ] || exit 1
printf 'rgbcheck: GOP half \033[32mPASS\033[0m — #rrggbb is bit-exact on the framebuffer\n'

# ───── BIOS/VGA half: the same probe, projected by dominant hue ─────

STRICT=on make run-bg >/dev/null 2>&1 || fail "make run-bg (BIOS) did not start"
run_rgbtest "BIOS"

VRAM="$ROOT/build/rgb_vram.txt"
./tools/qemu-input.sh raw "xp /4000bx 0xb8000" > "$VRAM" 2>/dev/null
grep -q "0x" "$VRAM" || fail "BIOS: VRAM dump came back empty"

python3 - "$VRAM" <<'VGAEOF'
import re, sys

rows = []
for ln in open(sys.argv[1]):
    m = re.match(r'^([0-9a-f]{8,}):\s+(.*)$', ln.strip())
    if not m:
        continue
    rows += [int(x, 16) for x in re.findall(r'0x([0-9a-f]{2})', m.group(2))]
if len(rows) < 4000:
    print(f'rgbcheck: FAIL VGA dump too short ({len(rows)} bytes)')
    sys.exit(1)
attrs = rows[1::2]

def attr(row, col):
    return attrs[row * 80 + col]

# The dominant-hue projection of the same rgbtest probe (see boxos_color.h):
#   #FF0000→RED #00FF00→GREEN #0000FF→BLUE #102030→BLUE (dark navy stays
#   blue) #FFB040→YELLOW #FFFFFF→WHITE; bg #402060→MAGENTA, #123456→BLUE;
#   fg #FEDCBA→WHITE (low saturation), default fg→LIGHT_GRAY.
probes = [
    # (row, col, want_attr, label)
    (1,  1, 0x5F, 'printf-path #FEDCBA on #402060 -> white on magenta'),
    (1,  9, 0x17, 'printf-path default on #123456 -> gray on blue'),
    (7,  1, 0x04, 'direct #FF0000 -> RED'),
    (7,  5, 0x02, 'direct #00FF00 -> GREEN'),
    (7,  9, 0x01, 'direct #0000FF -> BLUE'),
    (7, 13, 0x01, 'direct #102030 -> BLUE (hue, not black)'),
    (7, 17, 0x0E, 'direct #FFB040 -> YELLOW'),
    (7, 21, 0x0F, 'direct #FFFFFF -> WHITE'),
    (8,  1, 0x5F, 'direct bg #402060 -> MAGENTA (hue, not gray)'),
    (8,  5, 0x1F, 'direct bg #123456 -> BLUE'),
]

bad = 0
for row, col, want, label in probes:
    got = attr(row, col)
    if got == want:
        print(f'rgbcheck: PASS VGA {label} at cell ({row},{col})')
    else:
        print(f'rgbcheck: FAIL VGA {label} at cell ({row},{col}): '
              f'attr 0x{got:02X}, want 0x{want:02X}')
        bad += 1
sys.exit(1 if bad else 0)
VGAEOF
rc_vga=$?

make run-stop >/dev/null 2>&1
[ $rc_vga -eq 0 ] || exit 1
printf 'rgbcheck: VGA half \033[32mPASS\033[0m — the projection keeps every hue\n'
printf 'rgbcheck: \033[32mALL PASS\033[0m — GOP exact, VGA hue-faithful, both from one probe\n'
