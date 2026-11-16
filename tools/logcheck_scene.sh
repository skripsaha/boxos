# logcheck_scene — the scene scenario of tools/logcheck.sh: the brush
# (src/include/boxos_brush.h) Gangway and the display paint a scene with,
# built for the host from the very header under the address and
# undefined-behaviour sanitizers and held to what it must do
# (tools/scene_stand.c): exact areas, mirrors, text the press's very pixels,
# gradients, pictures, no pixel past the clip, what spills refused.
#
# SOURCED, NEVER RUN — by logcheck.sh, which dispatches `scene` to run_scene
# here; everything it calls (build, chk, harness) is stand.sh's.

run_scene() {
    echo "== scene: the brush held to exact areas, mirrors, the press's pixels and its clip =="
    build
    local stand="$SCRATCH/scene_stand" out="$SCRATCH/scene.stand" rc
    if ! cc -std=gnu11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-sanitize-recover=undefined \
        -Isrc/include -Ibuild/imprint_data tools/scene_stand.c -o "$stand" -lm >"$SCRATCH/scene.cc" 2>&1; then
        harness "scene: the stand did not build: $(head -3 "$SCRATCH/scene.cc" | tr '\n' ' ')"
        return
    fi
    "$stand" brush "$SCRATCH/scene.brush.png" >"$out" 2>&1
    rc=$?
    sed 's/^/  /' "$out"
    [ $rc = 0 ] && grep -qE "^scene: [0-9]+ passed, 0 failed — the brush$" "$out"
    chk $? "scene (brush): the brush built on the host holds to every check ($(tail -1 "$out")), the sheet at $SCRATCH/scene.brush.png"
}
