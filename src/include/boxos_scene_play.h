/*
 * boxos_scene_play.h — a scene (boxos_scene.h) played onto a canvas
 * (boxos_brush.h): every figure posed at a moment of an act, laid on the
 * canvas through its groups, and drawn where the picture changed since the
 * frame drawn last. Gangway draws the first frame of enter and goes on; the
 * display draws that frame again — the very pixels, so nothing on the glass
 * moves — and plays on from it.
 *
 * The rooms are the caller's: a pose and two lays for each figure, twice
 * over for the poses, and BOX_SCENE_EDGES edges.
 *
 * The includer supplies what boxos_scene.h asks.
 */
#ifndef BOXOS_SCENE_PLAY_H
#define BOXOS_SCENE_PLAY_H

#include "boxos_scene.h"

/* The most edges one figure is drawn with: the roundest shape, and a
 * rectangle's four to spare. */
#define BOX_SCENE_EDGES (BOX_BRUSH_STEPS_MOST + 8u)

typedef struct BoxScenePose {
    int32_t Value[BOX_SCENE_WHATS];
} BoxScenePose;

typedef struct BoxSceneLaid {
    BoxBrushMatrix World;     /* its own places onto the canvas */
    uint32_t       Opacity;   /* its own times its groups' */
    BoxBrushRect   Bounds;    /* the pixels it may cover: none for a group, an unseen figure, one the brush refuses */
} BoxSceneLaid;

typedef struct BoxScenePlayer {
    const BoxScene       *Scene;
    const BoxBrushCanvas *Canvas;
    BoxBrushMatrix        Screen;   /* the scene's places onto the canvas */
    BoxScenePose         *Start;    /* where each figure stood when the act began */
    BoxScenePose         *Now;
    BoxSceneLaid         *Laid;     /* the frame posed last */
    BoxSceneLaid         *Shown;    /* the frame drawn last */
    BoxBrushEdge         *Edges;
    uint32_t              Act;
    int                   Fresh;    /* nothing of the scene drawn yet */
} BoxScenePlayer;

/* The scene laid on the canvas — Units across its shorter side, its 0,0 the
 * canvas's middle — every figure where its Base stands, at the start of
 * enter. */
static inline void BoxScenePlayBegin(BoxScenePlayer *P, const BoxScene *S, const BoxBrushCanvas *K,
                                     BoxScenePose *Start, BoxScenePose *Now, BoxSceneLaid *Laid,
                                     BoxSceneLaid *Shown, BoxBrushEdge *Edges)
{
    const int64_t Short   = K->Width < K->Height ? K->Width : K->Height;
    const int32_t Stretch = BoxBrushNarrow(Short * BOX_BRUSH_UNIT / S->Head->Units);
    const BoxBrushMatrix Screen = { Stretch, 0, 0, Stretch, (int32_t)(K->Width * (BOX_BRUSH_ONE / 2)),
                                    (int32_t)(K->Height * (BOX_BRUSH_ONE / 2)) };
    P->Scene  = S;
    P->Canvas = K;
    P->Screen = Screen;
    P->Start  = Start;
    P->Now    = Now;
    P->Laid   = Laid;
    P->Shown  = Shown;
    P->Edges  = Edges;
    P->Act    = BOX_SCENE_ENTER;
    P->Fresh  = 1;
    for (uint32_t I = 0; I < S->Head->Figures; I++)
        for (uint32_t What = 0; What < BOX_SCENE_WHATS; What++)
            Start[I].Value[What] = Now[I].Value[What] = S->Figures[I].Base[What];
}

/* Act begun from where every figure stands now. */
static inline void BoxScenePlayAct(BoxScenePlayer *P, uint32_t Act)
{
    for (uint32_t I = 0; I < P->Scene->Head->Figures; I++) P->Start[I] = P->Now[I];
    P->Act = Act;
}

/* T, 0 to UNIT of the way between two keys, as Ease takes it. */
static inline int64_t BoxSceneEased(uint32_t Ease, int64_t T)
{
    const int64_t U = BOX_BRUSH_UNIT;
    switch (Ease) {
    case BOX_SCENE_EASE_IN: return T * T / U;
    case BOX_SCENE_EASE_OUT: return U - (U - T) * (U - T) / U;
    case BOX_SCENE_EASE_INOUT: return T * T / U * (3 * U - 2 * T) / U;
    case BOX_SCENE_EASE_STEP: return T >= U ? U : 0;
    default: return T;
    }
}

/* What track T makes of its figure At ms into the act, From where it stood
 * when the act began. */
static inline int32_t BoxSceneAlong(const BoxScene *S, const BoxSceneTrack *T, int32_t From, uint32_t At)
{
    const BoxSceneKey *Key   = &S->Keys[T->FirstKey];
    int64_t            Was   = From;
    uint32_t           WasAt = 0;
    for (uint32_t I = 0; I < T->KeyCount; I++) {
        if (At < Key[I].At) {
            const int64_t Part = (int64_t)(At - WasAt) * BOX_BRUSH_UNIT / (Key[I].At - WasAt);
            return (int32_t)(Was + BoxBrushRound((Key[I].Value - Was) * BoxSceneEased(Key[I].Ease, Part)));
        }
        Was   = Key[I].Value;
        WasAt = Key[I].At;
    }
    return (int32_t)Was;
}

/* The figure's own places onto its group's: turned and stretched about its
 * pivot, the pivot put at X, Y from where it was. */
static inline BoxBrushMatrix BoxSceneLocal(const BoxSceneFigure *F, const BoxScenePose *Pose)
{
    const int64_t  Cos  = BoxBrushCosine((uint32_t)Pose->Value[BOX_SCENE_TURN]);
    const int64_t  Sin  = BoxBrushSine((uint32_t)Pose->Value[BOX_SCENE_TURN]);
    const int64_t  Wide = Pose->Value[BOX_SCENE_WIDE], Tall = Pose->Value[BOX_SCENE_TALL];
    const int64_t  PX = F->Pivot[0], PY = F->Pivot[1];
    BoxBrushMatrix M;
    M.A = BoxBrushNarrow(BoxBrushRound(Cos * Wide));
    M.B = BoxBrushNarrow(BoxBrushRound(Sin * Wide));
    M.C = BoxBrushNarrow(BoxBrushRound(-Sin * Tall));
    M.D = BoxBrushNarrow(BoxBrushRound(Cos * Tall));
    M.E = BoxBrushNarrow(Pose->Value[BOX_SCENE_X] + PX - BoxBrushRound((int64_t)M.A * PX + (int64_t)M.C * PY));
    M.F = BoxBrushNarrow(Pose->Value[BOX_SCENE_Y] + PY - BoxBrushRound((int64_t)M.B * PX + (int64_t)M.D * PY));
    return M;
}

/* Figure I's outline built through its world, into the player's edges. */
static inline void BoxSceneOutline(const BoxScenePlayer *P, uint32_t I, BoxBrushOutline *O)
{
    const BoxSceneFigure *F = &P->Scene->Figures[I];
    BoxBrushBegin(O, P->Edges, BOX_SCENE_EDGES, &P->Laid[I].World);
    if (F->Kind == BOX_SCENE_ELLIPSE) BoxBrushAddEllipse(O, F->Shape[0], F->Shape[1], F->Shape[2], F->Shape[3], 0);
    else                              BoxBrushAddRect(O, F->Shape[0], F->Shape[1], F->Shape[2], F->Shape[3], 0);
    BoxBrushClose(O);
}

static inline int BoxSceneSame(const BoxSceneLaid *A, const BoxSceneLaid *B)
{
    return A->World.A == B->World.A && A->World.B == B->World.B && A->World.C == B->World.C &&
           A->World.D == B->World.D && A->World.E == B->World.E && A->World.F == B->World.F &&
           A->Opacity == B->Opacity && A->Bounds.X == B->Bounds.X && A->Bounds.Y == B->Bounds.Y &&
           A->Bounds.Width == B->Bounds.Width && A->Bounds.Height == B->Bounds.Height;
}

/* R among the Count rectangles in Dirty, Room of them at most: joined to every
 * one it meets, or kept apart, or — no room — joined to the first. */
static inline uint32_t BoxSceneDirty(BoxBrushRect *Dirty, uint32_t Count, uint32_t Room, BoxBrushRect R)
{
    for (uint32_t K = 0; K < Count;) {
        const BoxBrushRect Wider = { Dirty[K].X - 1, Dirty[K].Y - 1, Dirty[K].Width + 2, Dirty[K].Height + 2 };
        const BoxBrushRect Met   = BoxBrushMeet(Wider, R);
        if (Met.Width > 0 && Met.Height > 0) {
            R        = BoxBrushJoin(R, Dirty[K]);
            Dirty[K] = Dirty[--Count];
            K        = 0;
        } else {
            K++;
        }
    }
    if (Count < Room) {
        Dirty[Count++] = R;
    } else {
        Dirty[0] = BoxBrushJoin(Dirty[0], R);
    }
    return Count;
}

/*
 * Every figure posed Ms into the act — taken round the wait act's length,
 * held at the end of the others — and laid on the canvas. The rectangles of
 * the canvas the picture changed in since the frame drawn last, into Dirty,
 * Room at most and at least one: their count — the whole canvas before the
 * first frame is drawn.
 */
static inline uint32_t BoxScenePlayAt(BoxScenePlayer *P, uint32_t Ms, BoxBrushRect *Dirty, uint32_t Room)
{
    const BoxScene *S      = P->Scene;
    const uint32_t  Length = S->Head->Acts[P->Act];
    const uint32_t  At     = P->Act == BOX_SCENE_WAIT ? Ms % Length : Ms < Length ? Ms : Length;
    const BoxBrushRect None = { 0, 0, 0, 0 };
    BoxBrushCanvas     Whole = *P->Canvas;
    Whole.Clip.X = Whole.Clip.Y = 0;
    Whole.Clip.Width  = (int32_t)Whole.Width;
    Whole.Clip.Height = (int32_t)Whole.Height;

    for (uint32_t I = 0; I < S->Head->Figures; I++) P->Now[I] = P->Start[I];
    for (uint32_t K = 0; K < S->Head->Tracks; K++) {
        const BoxSceneTrack *T = &S->Tracks[K];
        if (T->Act != P->Act) continue;
        P->Now[T->Figure].Value[T->What] = BoxSceneAlong(S, T, P->Start[T->Figure].Value[T->What], At);
    }
    for (uint32_t I = 0; I < S->Head->Figures; I++) {
        const BoxSceneFigure *F    = &S->Figures[I];
        BoxSceneLaid         *Laid = &P->Laid[I];
        const BoxBrushMatrix  Over = F->Parent == BOX_SCENE_NONE ? P->Screen : P->Laid[F->Parent].World;
        const uint32_t        Seen = F->Parent == BOX_SCENE_NONE ? BOX_BRUSH_FULL : P->Laid[F->Parent].Opacity;
        Laid->World   = BoxBrushThen(BoxSceneLocal(F, &P->Now[I]), Over);
        Laid->Opacity = (Seen * (uint32_t)P->Now[I].Value[BOX_SCENE_OPACITY] + 127u) / BOX_BRUSH_FULL;
        Laid->Bounds  = None;
        if (F->Kind == BOX_SCENE_GROUP || Laid->Opacity == 0) continue;
        BoxBrushOutline O;
        BoxSceneOutline(P, I, &O);
        if (!O.Spilled) Laid->Bounds = BoxBrushReach(&Whole, &O);
    }

    if (P->Fresh) {
        Dirty[0] = Whole.Clip;
        return 1;
    }
    uint32_t Count = 0;
    for (uint32_t I = 0; I < S->Head->Figures; I++) {
        if (BoxSceneSame(&P->Laid[I], &P->Shown[I])) continue;
        const BoxBrushRect Changed = BoxBrushJoin(P->Shown[I].Bounds, P->Laid[I].Bounds);
        if (Changed.Width > 0 && Changed.Height > 0) Count = BoxSceneDirty(Dirty, Count, Room, Changed);
    }
    return Count;
}

/* The frame posed last drawn into each of the Count rectangles of the canvas:
 * the paper, and every figure that reaches into it, in their order. */
static inline void BoxScenePlayDraw(BoxScenePlayer *P, const BoxBrushRect *Rects, uint32_t Count)
{
    const BoxScene *S = P->Scene;
    BoxBrushCanvas  K = *P->Canvas;
    for (uint32_t R = 0; R < Count; R++) {
        const BoxBrushRect Whole = { 0, 0, (int32_t)K.Width, (int32_t)K.Height };
        K.Clip                   = BoxBrushMeet(Rects[R], Whole);
        BoxBrushClear(&K, K.Clip, S->Head->Paper);
        for (uint32_t I = 0; I < S->Head->Figures; I++) {
            const BoxSceneFigure *F    = &S->Figures[I];
            const BoxSceneLaid   *Laid = &P->Laid[I];
            const BoxBrushRect    Met  = BoxBrushMeet(Laid->Bounds, K.Clip);
            if (F->Kind == BOX_SCENE_GROUP || Laid->Opacity == 0 || Met.Width <= 0 || Met.Height <= 0) continue;
            const BoxScenePaint *Kept  = &S->Paints[F->Paint];
            const BoxBrushStop  *Stops = Kept->Kind == BOX_BRUSH_PAINT_SOLID ? NULL : &S->Stops[Kept->FirstStop];
            const BoxBrushPaint  Paint = { Kept->Kind, Kept->Color, Stops, Kept->StopCount,
                                           BoxBrushThen(Kept->Space, Laid->World), NULL, 0, 0, 0 };
            BoxBrushOutline      O;
            BoxSceneOutline(P, I, &O);
            BoxBrushFill(&K, &O, &Paint, Laid->Opacity);
        }
    }
    for (uint32_t I = 0; I < S->Head->Figures; I++) P->Shown[I] = P->Laid[I];
    P->Fresh = 0;
}

#endif /* BOXOS_SCENE_PLAY_H */
