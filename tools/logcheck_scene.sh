# logcheck_scene — the scene scenario of tools/logcheck.sh: the brush
# (src/include/boxos_brush.h) Gangway and the display paint a scene with, and
# the scene they play (boxos_scene.h, boxos_scene_play.h), built for the host
# from the very headers under the address and undefined-behaviour sanitizers
# and held to what they must do (tools/scene_stand.c): the brush to exact
# areas, mirrors, text the press's very pixels, gradients, pictures, its
# clip and what it refuses; every boot scene the volume carries, packed from
# its text (src/userspace/scenes/, tools/scene.py), to its first frame the
# same picture on every screen, every figure drawn whole on a screen of 8K,
# every frame drawn where it changed being the frame drawn whole, its acts
# meeting without a seam, and every way it and its paths may be damaged
# refused or played safely. Then on a machine: the glass Gangway leaves, dumped the
# moment it says the scene's first frame is on it, the stand's frame byte for
# byte, and the kernel told which scene; the display going on from that frame
# and leaving once the hub waits for the person, the console on the glass
# after; `settings gangway scene off` in the hub, and at the next start no
# scene is drawn, said, the firmware's picture left as it is, and the display
# plays none; `settings gangway scene boot light` then, and at the start after
# Gangway picks one of the light scenes by chance — said, the glass light —
# and the display plays on from the one picked.
#
# SOURCED, NEVER RUN — by logcheck.sh, which dispatches `scene` to run_scene
# here; everything it calls (build, chk, harness, the waits, the keys) is
# stand.sh's, stand_typer.sh's or logcheck.sh's.

# The pixels of two PPMs the same: 0, or 1.
scene_same() {
    python3 - "$1" "$2" <<'EOF'
import sys
def pixels(path):
    data, fields, at = open(path, "rb").read(), [], 0
    while len(fields) < 4:
        while data[at:at + 1].isspace():
            at += 1
        end = at
        while not data[end:end + 1].isspace():
            end += 1
        fields.append(data[at:end])
        at = end
    return fields[1:3], data[at + 1:]
sys.exit(0 if pixels(sys.argv[1]) == pixels(sys.argv[2]) else 1)
EOF
}

# A UEFI start, held still the moment Gangway says what became of the scene,
# its glass dumped into $SCRATCH/scene.TAG.glass.ppm — and left running.
scene_boot() {
    local tag=$1 i
    make run-stop >/dev/null 2>&1
    if ! make run-bg UEFI=on CORES=2 MEM=2G >"$SCRATCH/scene.$tag.run-bg" 2>&1; then
        harness "scene ($tag): run-bg would not start: $(tail -1 "$SCRATCH/scene.$tag.run-bg")"
        return 1
    fi
    for ((i = 0; i < WAIT_QUIET_S * 50; i++)); do
        grep -aqE "Gangway: (the scene's first frame is on the screen|no scene is drawn)" build/serial.log 2>/dev/null &&
            break
        sleep 0.02
    done
    mon_raw stop && mon_shot "$SCRATCH/scene.$tag.glass.ppm" && mon_raw cont
}

# Whether a PPM shows more than `least` pixels that are not black: 0, or 1.
scene_lit() {
    python3 - "$1" "$2" <<'EOF'
import sys
data, fields, at = open(sys.argv[1], "rb").read(), [], 0
while len(fields) < 4:
    while data[at:at + 1].isspace():
        at += 1
    end = at
    while not data[end:end + 1].isspace():
        end += 1
    fields.append(data[at:end])
    at = end
pixels = data[at + 1:]
lit = sum(1 for k in range(0, len(pixels), 3) if pixels[k:k + 3] != b"\0\0\0")
sys.exit(0 if lit > int(sys.argv[2]) else 1)
EOF
}

# Whether a PPM's middle pixel is not black — Duppo there, not the console's
# dark page: 0, or 1.
scene_middle() {
    python3 - "$1" <<'EOF'
import sys
data, fields, at = open(sys.argv[1], "rb").read(), [], 0
while len(fields) < 4:
    while data[at:at + 1].isspace():
        at += 1
    end = at
    while not data[end:end + 1].isspace():
        end += 1
    fields.append(data[at:end])
    at = end
width, height = int(fields[1]), int(fields[2])
middle = at + 1 + 3 * ((height // 2) * width + width // 2)
sys.exit(0 if data[middle:middle + 3] != b"\0\0\0" else 1)
EOF
}

# A line typed into the hub, Enter, and `until` waited for after it.
scene_say() {
    local from
    from=$(wc -l < build/serial.log | tr -d ' ')
    type_cmd "$1" || return 1
    sleep 1
    key_press ret || return 1
    wait_for_line build/serial.log "$2" "$WAIT_QUIET_S" $((from + 1)) && sleep 1
}

run_scene() {
    echo "== scene: the brush held to exact areas and its clip; the boot scene to its every frame and on the glass =="
    build
    local stand="$SCRATCH/scene_stand" out rc L size
    if ! cc -std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
        -Isrc/include -Ibuild/imprint_data tools/scene_stand.c -o "$stand" -lm >"$SCRATCH/scene.cc" 2>&1; then
        harness "scene: the stand did not build: $(head -3 "$SCRATCH/scene.cc" | tr '\n' ' ')"
        return
    fi

    out="$SCRATCH/scene.brush"
    "$stand" brush "$SCRATCH/scene.brush.png" >"$out" 2>&1
    rc=$?
    sed 's/^/  /' "$out"
    [ $rc = 0 ] && grep -qE "^scene: [0-9]+ passed, 0 failed — the brush$" "$out"
    chk $? "scene (brush): the brush built on the host holds to every check ($(tail -1 "$out")), the sheet at $SCRATCH/scene.brush.png"

    local one held=0 all=0 boot=build/scenes/dark-alive/boot.scene figures
    for one in build/scenes/*/*.scene; do
        out="$SCRATCH/scene.frames.$(basename "$(dirname "$one")").$(basename "$one" .scene)"
        rm -rf "$out.png" && mkdir -p "$out.png"
        "$stand" frames "$one" "$out.png" >"$out" 2>&1
        rc=$?
        echo "  -- $one"
        sed 's/^/  /' "$out"
        all=$((all + 1))
        [ $rc = 0 ] && grep -qE "^scene: [0-9]+ passed, 0 failed — the scene " "$out" && held=$((held + 1))
    done
    [ "$all" -gt 0 ] && [ "$held" = "$all" ]
    chk $? "scene (scenes): every boot scene the volume carries holds to every check ($held of $all), their frames in $SCRATCH/scene.frames.*.png"
    figures=$(python3 tools/scene.py read "$boot" | sed -nE 's/.* ([0-9]+) figures, .*/\1/p')

    cp build/boxos.img "$SCRATCH/scene.before.img"

    # The first start: the scene drawn by Gangway, the kernel told; then the
    # scene turned off in the hub for the starts after.
    if scene_boot first; then
        # A third of a second into enter, which is played to its end whatever
        # comes: the bubbles on their way out.
        local i
        for ((i = 0; i < WAIT_QUIET_S * 50; i++)); do
            grep -aq "\[display\] scene: the one the loader left" build/serial.log 2>/dev/null && break
            sleep 0.02
        done
        sleep 0.3
        mon_shot "$SCRATCH/scene.enter.glass.ppm"
        wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S"
        # The scene leaves once the hub waits for the person: the glass
        # looked at until it is the console, the scene gone and text on it.
        size=$(grep -a "Gangway: the scene's first frame is on the screen, " build/serial.log | tr -d '\r' | head -1 |
            sed -nE "s/.*on the screen, ([0-9]+)x([0-9]+) — .*/\1 \2/p")
        [ -n "$size" ] && "$stand" first "$boot" $size "$SCRATCH/scene.first.stand.ppm"
        for ((i = 0; i < 50; i++)); do
            mon_shot "$SCRATCH/scene.first.console.ppm" &&
                ! scene_same "$SCRATCH/scene.first.console.ppm" "$SCRATCH/scene.first.stand.ppm" &&
                scene_lit "$SCRATCH/scene.first.console.ppm" 500 && break
            sleep 0.2
        done
        scene_say "settings gangway scene off" "when it starts next"
        cp build/serial.log "$SCRATCH/serial.scene.first.log"
    fi
    L="$SCRATCH/serial.scene.first.log"
    size=$(grep -a "Gangway: the scene's first frame is on the screen, " "$L" | tr -d '\r' | head -1 |
        sed -nE "s/.*on the screen, ([0-9]+)x([0-9]+) — .*/\1 \2/p")
    grep -aq "Gangway: boot.scene is a scene of $figures figure(s)" "$L" && [ -n "$size" ]
    chk $? "scene (gangway): Gangway reads boot.scene by its name and draws its first frame ($(echo $size | tr ' ' x))"
    [ -n "$size" ] && scene_same "$SCRATCH/scene.first.glass.ppm" "$SCRATCH/scene.first.stand.ppm"
    chk $? "scene (gangway): the glass, dumped the moment Gangway says so, is the stand's first frame byte for byte"
    grep -aqE "\[Boarding\]   the loader left the first frame of its scene on the screen \(fid [0-9]+, [0-9]+ bytes\)" "$L"
    chk $? "scene (gangway): the kernel is told which scene the glass shows"
    grep -aqE "\[display\] scene: the one the loader left \(fid [0-9]+, $figures figures\) goes on from its frame, $(echo $size | tr ' ' x)" "$L"
    chk $? "scene (display): the display goes on from the frame the loader left, at the loader's size"
    [ -s "$SCRATCH/scene.enter.glass.ppm" ] &&
        ! scene_same "$SCRATCH/scene.enter.glass.ppm" "$SCRATCH/scene.first.stand.ppm" &&
        scene_middle "$SCRATCH/scene.enter.glass.ppm"
    chk $? "scene (display): it plays — a third of a second into enter the glass is neither the loader's frame nor the console"
    [ -s "$SCRATCH/scene.first.console.ppm" ] &&
        ! scene_same "$SCRATCH/scene.first.console.ppm" "$SCRATCH/scene.first.stand.ppm" &&
        scene_lit "$SCRATCH/scene.first.console.ppm" 500
    chk $? "scene (display): once the hub waits for the person the scene leaves — the glass is the console, not the scene, and not dark"
    grep -aq "gangway.settings" "$L" && grep -aq "when it starts next" "$L"
    chk $? "scene (off): settings gangway scene off, said to take effect when the machine starts next"

    # The next start, the scene off.
    if [ -s "$L" ] && mon_raw system_reset; then
        local i
        for ((i = 0; i < WAIT_QUIET_S * 50; i++)); do
            grep -aq "Gangway: no scene is drawn" build/serial.log 2>/dev/null && break
            sleep 0.02
        done
        mon_raw stop && mon_shot "$SCRATCH/scene.off.glass.ppm" && mon_raw cont
        wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" "$(grep -an "Gangway: no scene is drawn" build/serial.log | tail -1 | cut -d: -f1)"
        sleep 1
        cp build/serial.log "$SCRATCH/serial.scene.off.log"
        # Then the scene asked by tags, and the start after it.
        scene_say "settings gangway scene boot light" "when it starts next"
        local from
        from=$(wc -l < build/serial.log | tr -d ' ')
        if mon_raw system_reset; then
            for ((i = 0; i < WAIT_QUIET_S * 50; i++)); do
                tail -n +"$from" build/serial.log 2>/dev/null | grep -aq "Gangway: the scene's first frame is on the screen" && break
                sleep 0.02
            done
            mon_raw stop && mon_shot "$SCRATCH/scene.tags.glass.ppm" && mon_raw cont
            wait_for_line build/serial.log "BoxOS Hub" "$WAIT_QUIET_S" "$from"
            sleep 1
            tail -n +"$from" build/serial.log > "$SCRATCH/serial.scene.tags.log"
        fi
    fi
    make run-stop >/dev/null 2>&1
    L="$SCRATCH/serial.scene.off.log"
    grep -aq "Gangway: gangway.settings sets scene off" "$L" && grep -aq "Gangway: no scene is drawn — the scene is off" "$L"
    chk $? "scene (off): the next start reads scene off and says no scene is drawn"
    [ -s "$SCRATCH/scene.off.glass.ppm" ] && ! scene_same "$SCRATCH/scene.off.glass.ppm" "$SCRATCH/scene.first.stand.ppm" &&
        [ "$(grep -ac "the loader left the first frame of its scene" "$L")" = 1 ] &&
        [ "$(grep -ac "\[display\] scene: the one the loader left" "$L")" = 1 ] && grep -aq "BoxOS Hub" "$L"
    chk $? "scene (off): the glass is not the scene, neither the kernel nor the display hears of one, and the start reaches the hub"

    L="$SCRATCH/serial.scene.tags.log"
    local picked
    picked=$(sed -nE 's/^Gangway: [0-9]+ scenes carry boot, light — fid ([0-9]+), the one picked, is read.*/\1/p' "$L" 2>/dev/null | head -1)
    grep -aq "Gangway: gangway.settings sets scene boot light" "$L" && [ -n "$picked" ] &&
        grep -aq "Gangway: the scene picked, fid $picked, is a scene of " "$L"
    chk $? "scene (tags): asked by boot and light, Gangway picks one of the scenes that wear both — fid ${picked:-none} — and reads it"
    [ -s "$SCRATCH/scene.tags.glass.ppm" ] && python3 - "$SCRATCH/scene.tags.glass.ppm" <<'EOF'
import sys
data, fields, at = open(sys.argv[1], "rb").read(), [], 0
while len(fields) < 4:
    while data[at:at + 1].isspace():
        at += 1
    end = at
    while not data[end:end + 1].isspace():
        end += 1
    fields.append(data[at:end])
    at = end
sys.exit(0 if data[at + 1:at + 4] == b"\xff\xff\xff" else 1)
EOF
    chk $? "scene (tags): the glass, dumped the moment Gangway says so, is a light scene's — its corner the white paper"
    grep -aqE "\[display\] scene: the one the loader left \(fid ${picked:-none}, [0-9]+ figures\) goes on from its frame" "$L"
    chk $? "scene (tags): the display goes on from the scene picked, by its fid"

    cp "$SCRATCH/scene.before.img" build/boxos.img
}
