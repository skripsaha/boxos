/*
 * boxos_scene.h — a scene: a picture that moves, the whole screen's, in the
 * form every hand that plays one reads — Gangway, which shows its first frame
 * while the machine starts, the display, which goes on from that frame
 * (boxos_scene_play.h), and the host stand (tools/scene_stand.c).
 * tools/scene.py packs one from its text (src/userspace/scenes/).
 *
 * A scene is figures laid one over another in their order — groups, which
 * draw nothing and carry the figures in them, and shapes painted as the brush
 * paints (boxos_brush.h) — and tracks that move them through three acts:
 * enter, played once; wait, played round and round while the machine is not
 * ready for the person; leave, played once, and then the scene is gone. Its
 * places are 1/BOX_BRUSH_ONE of its units, Units of them across the screen's
 * shorter side, 0,0 the screen's middle, y down: the same picture on every
 * screen.
 *
 * A figure stands at X, Y of its group, turned by TURN and stretched by WIDE
 * and TALL about its pivot; its opacity is its own times its group's. A track
 * moves one of those for one figure through one act, by keys: before its
 * first key from where the figure stood when the act began, between keys
 * eased as the later key says, after its last as the last says; the wait
 * act's keys are taken round its length. Packed, the wait act's tracks begin
 * where enter left each figure and end where they began, so the round has no
 * seam (tools/scene.py holds a scene to it).
 *
 * The file is the head and then its five tables, each right after the one
 * before, little-endian: paints, stops, figures, tracks, keys. It is read
 * whole and held to every bound before anything is drawn (BoxSceneRead): a
 * scene that is not one is refused with why, and nothing past the bytes
 * handed in is read, whatever they hold. No version: what a reader does not
 * know it refuses.
 *
 * The includer supplies uint8_t, uint32_t, int32_t, uint64_t, int64_t, size_t
 * and NULL.
 */
#ifndef BOXOS_SCENE_H
#define BOXOS_SCENE_H

#include "boxos_brush.h"

#define BOX_SCENE_MAGIC "BOXSCENE"

/* The scene the machine starts with, as it is called on the volume — found by
 * its name, never by a tag — and the most bytes of a scene a loader reads. */
#define BOX_SCENE_BOOT      "boot.scene"
#define BOX_SCENE_FILE_MOST (1u << 20)

#define BOX_SCENE_ENTER 0u
#define BOX_SCENE_WAIT  1u
#define BOX_SCENE_LEAVE 2u
#define BOX_SCENE_ACTS  3u

/* The longest an act may be, in milliseconds: an hour. */
#define BOX_SCENE_ACT_MOST 3600000u

#define BOX_SCENE_UNITS_LEAST 16u
#define BOX_SCENE_UNITS_MOST  65536u

#define BOX_SCENE_GROUP   0u
#define BOX_SCENE_ELLIPSE 1u   /* Shape: CX, CY, RX, RY */
#define BOX_SCENE_RECT    2u   /* Shape: X, Y, WIDTH, HEIGHT */

/* What a track moves, and where a figure's Base keeps it. */
#define BOX_SCENE_X       0u   /* places of its group */
#define BOX_SCENE_Y       1u
#define BOX_SCENE_WIDE    2u   /* 1/BOX_BRUSH_UNIT, 0 to BOX_BRUSH_STRETCH_MOST times it */
#define BOX_SCENE_TALL    3u
#define BOX_SCENE_TURN    4u   /* 1/BOX_BRUSH_TURN of a turn, as many turns as it likes */
#define BOX_SCENE_OPACITY 5u   /* 0 to 255 */
#define BOX_SCENE_WHATS   6u

/* How a key is come to from the one before it. */
#define BOX_SCENE_EASE_LINEAR 0u
#define BOX_SCENE_EASE_IN     1u   /* slow to start */
#define BOX_SCENE_EASE_OUT    2u   /* slow to stop */
#define BOX_SCENE_EASE_INOUT  3u
#define BOX_SCENE_EASE_STEP   4u   /* none of the way until the key */
#define BOX_SCENE_EASES       5u

#define BOX_SCENE_NONE 0xFFFFFFFFu   /* a figure in no group */

typedef struct BoxSceneHead {
    char     Magic[8];
    uint32_t Size;                    /* the file's bytes */
    uint32_t Units;
    uint32_t Paper;                   /* 0x00RRGGBB: what lies under every figure */
    uint32_t Acts[BOX_SCENE_ACTS];    /* each act's length, ms */
    uint32_t Paints, Stops, Figures, Tracks, Keys;
} BoxSceneHead;

/* As the brush paints (BoxBrushPaint): SOLID, LINEAR or RADIAL, the stops
 * FirstStop on, Space laying the paint's own space on the figure's. */
typedef struct BoxScenePaint {
    uint32_t       Kind;
    uint32_t       Color;
    uint32_t       FirstStop, StopCount;
    BoxBrushMatrix Space;
} BoxScenePaint;

typedef struct BoxSceneFigure {
    uint32_t Kind;
    uint32_t Parent;                   /* a group laid before it, or NONE */
    uint32_t Paint;                    /* ELLIPSE, RECT */
    int32_t  Shape[4];                 /* its own places */
    int32_t  Pivot[2];                 /* what it turns and stretches about, in its own places */
    int32_t  Base[BOX_SCENE_WHATS];    /* where it stands before any track moves it */
} BoxSceneFigure;

/* Tracks are kept by act, then figure, then what they move — one at most for
 * each. */
typedef struct BoxSceneTrack {
    uint32_t Act;
    uint32_t Figure;
    uint32_t What;
    uint32_t FirstKey, KeyCount;
} BoxSceneTrack;

typedef struct BoxSceneKey {
    uint32_t At;      /* ms into the act, rising */
    int32_t  Value;
    uint32_t Ease;
} BoxSceneKey;

#ifdef __cplusplus
static_assert(sizeof(BoxSceneHead) == 52 && sizeof(BoxScenePaint) == 40 && sizeof(BoxBrushStop) == 8 &&
                  sizeof(BoxSceneFigure) == 60 && sizeof(BoxSceneTrack) == 20 && sizeof(BoxSceneKey) == 12,
              "a scene's records are the sizes tools/scene.py packs");
#else
_Static_assert(sizeof(BoxSceneHead) == 52 && sizeof(BoxScenePaint) == 40 && sizeof(BoxBrushStop) == 8 &&
                   sizeof(BoxSceneFigure) == 60 && sizeof(BoxSceneTrack) == 20 && sizeof(BoxSceneKey) == 12,
               "a scene's records are the sizes tools/scene.py packs");
#endif

/* A scene read and held to its bounds: its tables, in the bytes it was read
 * from. Fault: the record a refusal is about, counted from 0 in its table. */
typedef struct BoxScene {
    const BoxSceneHead   *Head;
    const BoxScenePaint  *Paints;
    const BoxBrushStop   *Stops;
    const BoxSceneFigure *Figures;
    const BoxSceneTrack  *Tracks;
    const BoxSceneKey    *Keys;
    uint32_t              Fault;
} BoxScene;

static inline int BoxSceneOwn(int64_t Value)
{
    return BoxBrushSize(Value) <= BOX_BRUSH_OWN_MOST;
}

/* Whether Value is one a figure's What may hold. */
static inline int BoxSceneValue(uint32_t What, int32_t Value)
{
    switch (What) {
    case BOX_SCENE_X:
    case BOX_SCENE_Y: return BoxSceneOwn(Value);
    case BOX_SCENE_WIDE:
    case BOX_SCENE_TALL: return Value >= 0 && Value <= BOX_BRUSH_STRETCH_MOST * BOX_BRUSH_UNIT;
    case BOX_SCENE_TURN: return 1;
    default: return Value >= 0 && Value <= (int32_t)BOX_BRUSH_FULL;
    }
}

static inline const char *BoxScenePaintFault(const BoxScene *S, const BoxScenePaint *P)
{
    if (P->Kind == BOX_BRUSH_PAINT_SOLID) return NULL;
    if (P->Kind != BOX_BRUSH_PAINT_LINEAR && P->Kind != BOX_BRUSH_PAINT_RADIAL) return "a paint of no kind a scene has";
    if (P->StopCount == 0 || (uint64_t)P->FirstStop + P->StopCount > S->Head->Stops)
        return "a gradient's stops lie past the stops kept";
    for (uint32_t K = 0; K < P->StopCount; K++) {
        const BoxBrushStop *Stop = &S->Stops[P->FirstStop + K];
        if (Stop->At > BOX_BRUSH_UNIT || (K && Stop->At < S->Stops[P->FirstStop + K - 1u].At))
            return "a gradient's stops do not rise from 0 to 1";
    }
    if (!BoxBrushFits(&P->Space) || (int64_t)P->Space.A * P->Space.D == (int64_t)P->Space.B * P->Space.C)
        return "a gradient's space folds flat or reaches past what the brush draws";
    return NULL;
}

static inline const char *BoxSceneFigureFault(const BoxScene *S, uint32_t Index)
{
    const BoxSceneFigure *F = &S->Figures[Index];
    if (F->Kind > BOX_SCENE_RECT) return "a figure of no kind a scene has";
    if (F->Parent != BOX_SCENE_NONE && (F->Parent >= Index || S->Figures[F->Parent].Kind != BOX_SCENE_GROUP))
        return "a figure's group is not a group laid before it";
    if (F->Kind != BOX_SCENE_GROUP) {
        if (F->Paint >= S->Head->Paints) return "a figure is painted with no paint the scene keeps";
        for (uint32_t K = 0; K < 4u; K++)
            if (!BoxSceneOwn(F->Shape[K])) return "a figure's shape reaches past what the brush draws";
        if (F->Shape[2] <= 0 || F->Shape[3] <= 0) return "a figure's shape has no size";
    }
    if (!BoxSceneOwn(F->Pivot[0]) || !BoxSceneOwn(F->Pivot[1])) return "a figure's pivot lies past what the brush draws";
    for (uint32_t What = 0; What < BOX_SCENE_WHATS; What++)
        if (!BoxSceneValue(What, F->Base[What])) return "a figure stands where none may";
    return NULL;
}

static inline const char *BoxSceneTrackFault(const BoxScene *S, uint32_t Index)
{
    const BoxSceneTrack *T = &S->Tracks[Index];
    if (T->Act >= BOX_SCENE_ACTS || T->Figure >= S->Head->Figures || T->What >= BOX_SCENE_WHATS)
        return "a track moves what the scene has not";
    if (Index) {
        const BoxSceneTrack *B = &S->Tracks[Index - 1u];
        const uint64_t       Was = ((uint64_t)B->Act << 40) | ((uint64_t)B->Figure << 8) | B->What;
        const uint64_t       Is  = ((uint64_t)T->Act << 40) | ((uint64_t)T->Figure << 8) | T->What;
        if (Is <= Was) return "the tracks are not kept by act, figure and what, one for each";
    }
    if (T->KeyCount == 0 || (uint64_t)T->FirstKey + T->KeyCount > S->Head->Keys) return "a track's keys lie past the keys kept";
    for (uint32_t K = 0; K < T->KeyCount; K++) {
        const BoxSceneKey *Key = &S->Keys[T->FirstKey + K];
        if (Key->At > S->Head->Acts[T->Act] || (K && Key->At < S->Keys[T->FirstKey + K - 1u].At))
            return "a track's keys do not rise within its act";
        if (Key->Ease >= BOX_SCENE_EASES || !BoxSceneValue(T->What, Key->Value)) return "a track's key is none a scene has";
    }
    return NULL;
}

/*
 * The scene in Size bytes at Bytes, read into S and held to every bound:
 * NULL, or why it is refused, S->Fault the record that is wrong. The bytes
 * stay the caller's; S points into them.
 */
static inline const char *BoxSceneRead(const uint8_t *Bytes, uint64_t Size, BoxScene *S)
{
    S->Fault = 0;
    if (Size < sizeof(BoxSceneHead)) return "it is shorter than a scene's head";
    S->Head = (const BoxSceneHead *)Bytes;
    const BoxSceneHead *H = S->Head;
    for (uint32_t I = 0; I < 8u; I++)
        if (H->Magic[I] != BOX_SCENE_MAGIC[I]) return "it does not begin as a scene does";
    const uint64_t Paints  = sizeof(BoxSceneHead);
    const uint64_t Stops   = Paints + (uint64_t)H->Paints * sizeof(BoxScenePaint);
    const uint64_t Figures = Stops + (uint64_t)H->Stops * sizeof(BoxBrushStop);
    const uint64_t Tracks  = Figures + (uint64_t)H->Figures * sizeof(BoxSceneFigure);
    const uint64_t Keys    = Tracks + (uint64_t)H->Tracks * sizeof(BoxSceneTrack);
    const uint64_t End     = Keys + (uint64_t)H->Keys * sizeof(BoxSceneKey);
    if (End != H->Size || H->Size != Size) return "its size is not the size of its tables";
    if (H->Units < BOX_SCENE_UNITS_LEAST || H->Units > BOX_SCENE_UNITS_MOST) return "its units are past what a screen holds";
    if (H->Paper > 0x00FFFFFFu) return "its paper is no colour";
    for (uint32_t A = 0; A < BOX_SCENE_ACTS; A++)
        if (H->Acts[A] > BOX_SCENE_ACT_MOST) return "an act is longer than an hour";
    if (H->Acts[BOX_SCENE_WAIT] == 0) return "its wait act has no length to go round";
    if (H->Figures == 0) return "it has no figure";
    S->Paints  = (const BoxScenePaint *)(Bytes + Paints);
    S->Stops   = (const BoxBrushStop *)(Bytes + Stops);
    S->Figures = (const BoxSceneFigure *)(Bytes + Figures);
    S->Tracks  = (const BoxSceneTrack *)(Bytes + Tracks);
    S->Keys    = (const BoxSceneKey *)(Bytes + Keys);
    for (S->Fault = 0; S->Fault < H->Paints; S->Fault++) {
        const char *Why = BoxScenePaintFault(S, &S->Paints[S->Fault]);
        if (Why) return Why;
    }
    for (S->Fault = 0; S->Fault < H->Figures; S->Fault++) {
        const char *Why = BoxSceneFigureFault(S, S->Fault);
        if (Why) return Why;
    }
    for (S->Fault = 0; S->Fault < H->Tracks; S->Fault++) {
        const char *Why = BoxSceneTrackFault(S, S->Fault);
        if (Why) return Why;
    }
    S->Fault = 0;
    return NULL;
}

#endif /* BOXOS_SCENE_H */
