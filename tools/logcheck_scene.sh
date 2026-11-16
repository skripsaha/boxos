# logcheck_scene — the scene scenario of tools/logcheck.sh: the brush
# (src/include/boxos_brush.h) Gangway and the display paint a scene with, and
# the scene they play (boxos_scene.h, boxos_scene_play.h), built for the host
# from the very headers under the address and undefined-behaviour sanitizers
# and held to what they must do (tools/scene_stand.c): the brush to exact
# areas, mirrors, text the press's very pixels, gradients, pictures, its
# clip and what it refuses; the boot scene packed from its text
# (src/userspace/scenes/boot.txt, tools/scene.py) to its first frame on every
# screen, every frame drawn where it changed being the frame drawn whole, its
# acts meeting without a seam, and every way it may be damaged refused or
# played safely.
#
# SOURCED, NEVER RUN — by logcheck.sh, which dispatches `scene` to run_scene
# here; everything it calls (build, chk, harness) is stand.sh's.

run_scene() {
    echo "== scene: the brush held to exact areas and its clip; the boot scene to its every frame =="
    build
    local stand="$SCRATCH/scene_stand" out rc
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

    out="$SCRATCH/scene.frames"
    rm -rf "$SCRATCH/scene.frames.png" && mkdir -p "$SCRATCH/scene.frames.png"
    if ! python3 tools/scene.py pack src/userspace/scenes/boot.txt "$SCRATCH/boot.scene" >"$out.pack" 2>&1; then
        chk 1 "scene (boot): boot.txt packs ($(tail -1 "$out.pack"))"
        return
    fi
    "$stand" frames "$SCRATCH/boot.scene" "$SCRATCH/scene.frames.png" >"$out" 2>&1
    rc=$?
    sed 's/^/  /' "$out"
    [ $rc = 0 ] && grep -qE "^scene: [0-9]+ passed, 0 failed — the scene " "$out"
    chk $? "scene (boot): the boot scene holds to every check ($(tail -1 "$out" | sed 's/^scene: //; s/ — .*//')), its frames in $SCRATCH/scene.frames.png"
}
