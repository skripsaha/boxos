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

/* The most edges one figure is drawn with: four times the roundest ellipse —
 * a path's curves are broken by how far they bend on the canvas, and a
 * letter of a word across a screen of 8K takes some hundreds. A figure that
 * would take more is not drawn (its outline spills); the stand holds every
 * scene the machine carries to drawing whole on such a screen. */
#define BOX_SCENE_EDGES (4u * BOX_SHAPE_STEPS_MOST)

typedef struct BoxScenePose {
    int32_t Value[BOX_SCENE_WHATS];
} BoxScenePose;

typedef struct BoxSceneLaid {
    BoxMatrix      World;     /* its own places onto the canvas */
    uint32_t       Opacity;   /* its own times its groups' */
    BoxRect        Bounds;    /* the pixels it may cover: none for a group, an unseen figure, one the brush refuses */
} BoxSceneLaid;

typedef struct BoxScenePlayer {
    const BoxScene       *Scene;
    const BoxBrushCanvas *Canvas;
    BoxMatrix             Screen;   /* the scene's places onto the canvas */
    BoxScenePose         *Start;    /* where each figure stood when the act began */
    BoxScenePose         *Now;
    BoxSceneLaid         *Laid;     /* the frame posed last */
    BoxSceneLaid         *Shown;    /* the frame drawn last */
    BoxShapeEdge         *Edges;
    uint32_t              Act;
    int                   Fresh;    /* nothing of the scene drawn yet */
} BoxScenePlayer;

/* The scene laid on the canvas — Units across its shorter side, its 0,0 the
 * canvas's middle — every figure where its Base stands, at the start of
 * enter. */
static inline void BoxScenePlayBegin(BoxScenePlayer *P, const BoxScene *S, const BoxBrushCanvas *K,
                                     BoxScenePose *Start, BoxScenePose *Now, BoxSceneLaid *Laid,
                                     BoxSceneLaid *Shown, BoxShapeEdge *Edges)
{
    const int64_t Short   = K->Width < K->Height ? K->Width : K->Height;
    const int32_t Stretch = BoxShapeNarrow(Short * BOX_SHAPE_UNIT / S->Head->Units);
    const BoxMatrix Screen = { Stretch, 0, 0, Stretch, (int32_t)(K->Width * (BOX_SHAPE_ONE / 2)),
                               (int32_t)(K->Height * (BOX_SHAPE_ONE / 2)) };
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
    const int64_t U = BOX_SHAPE_UNIT;
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
            const int64_t Part = (int64_t)(At - WasAt) * BOX_SHAPE_UNIT / (Key[I].At - WasAt);
            return (int32_t)(Was + BoxShapeRound((Key[I].Value - Was) * BoxSceneEased(Key[I].Ease, Part)));
        }
        Was   = Key[I].Value;
        WasAt = Key[I].At;
    }
    return (int32_t)Was;
}

/* The figure's own places onto its group's: turned and stretched about its
 * pivot, the pivot put at X, Y from where it was. */
static inline BoxMatrix BoxSceneLocal(const BoxSceneFigure *F, const BoxScenePose *Pose)
{
    const int64_t Cos  = BoxShapeCosine((uint32_t)Pose->Value[BOX_SCENE_TURN]);
    const int64_t Sin  = BoxShapeSine((uint32_t)Pose->Value[BOX_SCENE_TURN]);
    const int64_t Wide = Pose->Value[BOX_SCENE_WIDE], Tall = Pose->Value[BOX_SCENE_TALL];
    const int64_t PX = F->Pivot[0], PY = F->Pivot[1];
    BoxMatrix     M;
    M.A = BoxShapeNarrow(BoxShapeRound(Cos * Wide));
    M.B = BoxShapeNarrow(BoxShapeRound(Sin * Wide));
    M.C = BoxShapeNarrow(BoxShapeRound(-Sin * Tall));
    M.D = BoxShapeNarrow(BoxShapeRound(Cos * Tall));
    M.E = BoxShapeNarrow(Pose->Value[BOX_SCENE_X] + PX - BoxShapeRound((int64_t)M.A * PX + (int64_t)M.C * PY));
    M.F = BoxShapeNarrow(Pose->Value[BOX_SCENE_Y] + PY - BoxShapeRound((int64_t)M.B * PX + (int64_t)M.D * PY));
    return M;
}

/* Figure I's outline built through its world, into the player's edges. */
/* A path's pieces followed by the pen. */
static inline void BoxSceneAddPath(const BoxScene *S, const BoxSceneFigure *F, BoxShape *O)
{
    const BoxScenePiece *P = &S->Pieces[F->Shape[0]];
    for (uint32_t K = 0; K < (uint32_t)F->Shape[1];) {
        switch (P[K].Verb) {
        case BOX_SCENE_MOVE: BoxShapeMoveTo(O, P[K].X, P[K].Y); K += 1u; break;
        case BOX_SCENE_LINE: BoxShapeLineTo(O, P[K].X, P[K].Y); K += 1u; break;
        case BOX_SCENE_QUAD: BoxShapeQuadTo(O, P[K].X, P[K].Y, P[K + 1u].X, P[K + 1u].Y); K += 2u; break;
        default:
            BoxShapeCubicTo(O, P[K].X, P[K].Y, P[K + 1u].X, P[K + 1u].Y, P[K + 2u].X, P[K + 2u].Y);
            K += 3u;
            break;
        }
    }
}

static inline void BoxSceneOutline(const BoxScenePlayer *P, uint32_t I, BoxShape *O)
{
    const BoxSceneFigure *F = &P->Scene->Figures[I];
    BoxShapeBegin(O, P->Edges, BOX_SCENE_EDGES, &P->Laid[I].World);
    if (F->Kind == BOX_SCENE_ELLIPSE)   BoxShapeAddEllipse(O, F->Shape[0], F->Shape[1], F->Shape[2], F->Shape[3], 0);
    else if (F->Kind == BOX_SCENE_RECT) BoxShapeAddRect(O, F->Shape[0], F->Shape[1], F->Shape[2], F->Shape[3], 0);
    else                                BoxSceneAddPath(P->Scene, F, O);
    BoxShapeClose(O);
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
static inline uint32_t BoxSceneDirty(BoxRect *Dirty, uint32_t Count, uint32_t Room, BoxRect R)
{
    for (uint32_t K = 0; K < Count;) {
        const BoxRect Wider = { Dirty[K].X - 1, Dirty[K].Y - 1, Dirty[K].Width + 2, Dirty[K].Height + 2 };
        const BoxRect Met   = BoxRectMeet(Wider, R);
        if (Met.Width > 0 && Met.Height > 0) {
            R        = BoxRectJoin(R, Dirty[K]);
            Dirty[K] = Dirty[--Count];
            K        = 0;
        } else {
            K++;
        }
    }
    if (Count < Room) {
        Dirty[Count++] = R;
    } else {
        Dirty[0] = BoxRectJoin(Dirty[0], R);
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
static inline uint32_t BoxScenePlayAt(BoxScenePlayer *P, uint32_t Ms, BoxRect *Dirty, uint32_t Room)
{
    const BoxScene *S      = P->Scene;
    const uint32_t  Length = S->Head->Acts[P->Act];
    const uint32_t  At     = P->Act == BOX_SCENE_WAIT ? Ms % Length : Ms < Length ? Ms : Length;
    const BoxRect None = { 0, 0, 0, 0 };
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
        const BoxMatrix       Over = F->Parent == BOX_SCENE_NONE ? P->Screen : P->Laid[F->Parent].World;
        const uint32_t        Seen = F->Parent == BOX_SCENE_NONE ? BOX_SHAPE_FULL : P->Laid[F->Parent].Opacity;
        Laid->World   = BoxMatrixThen(BoxSceneLocal(F, &P->Now[I]), Over);
        Laid->Opacity = (Seen * (uint32_t)P->Now[I].Value[BOX_SCENE_OPACITY] + 127u) / BOX_SHAPE_FULL;
        Laid->Bounds  = None;
        if (F->Kind == BOX_SCENE_GROUP || Laid->Opacity == 0) continue;
        BoxShape O;
        BoxSceneOutline(P, I, &O);
        if (BoxShapeDrawable(&O)) Laid->Bounds = BoxShapeReach(&O, BoxBrushWithin(&Whole));
    }

    if (P->Fresh) {
        Dirty[0] = Whole.Clip;
        return 1;
    }
    uint32_t Count = 0;
    for (uint32_t I = 0; I < S->Head->Figures; I++) {
        if (BoxSceneSame(&P->Laid[I], &P->Shown[I])) continue;
        const BoxRect Changed = BoxRectJoin(P->Shown[I].Bounds, P->Laid[I].Bounds);
        if (Changed.Width > 0 && Changed.Height > 0) Count = BoxSceneDirty(Dirty, Count, Room, Changed);
    }
    return Count;
}

/* The frame posed last drawn into each of the Count rectangles of the canvas:
 * the paper, and every figure that reaches into it, in their order. */
static inline void BoxScenePlayDraw(BoxScenePlayer *P, const BoxRect *Rects, uint32_t Count)
{
    const BoxScene *S = P->Scene;
    BoxBrushCanvas  K = *P->Canvas;
    for (uint32_t R = 0; R < Count; R++) {
        const BoxRect Whole = { 0, 0, (int32_t)K.Width, (int32_t)K.Height };
        K.Clip              = BoxRectMeet(Rects[R], Whole);
        BoxBrushClear(&K, K.Clip, S->Head->Paper);
        for (uint32_t I = 0; I < S->Head->Figures; I++) {
            const BoxSceneFigure *F    = &S->Figures[I];
            const BoxSceneLaid   *Laid = &P->Laid[I];
            const BoxRect         Met  = BoxRectMeet(Laid->Bounds, K.Clip);
            if (F->Kind == BOX_SCENE_GROUP || Laid->Opacity == 0 || Met.Width <= 0 || Met.Height <= 0) continue;
            const BoxScenePaint *Kept  = &S->Paints[F->Paint];
            const BoxPaintStop  *Stops = Kept->Kind == BOX_PAINT_SOLID ? NULL : &S->Stops[Kept->FirstStop];
            const BoxPaint       Paint = { Kept->Kind, Kept->Color, Stops, Kept->StopCount,
                                           BoxMatrixThen(Kept->Space, Laid->World), NULL, 0, 0, 0, BOX_PAINT_PAD, 0, 0, 0 };
            BoxShape             O;
            BoxSceneOutline(P, I, &O);
            BoxBrushFill(&K, &O, &Paint, Laid->Opacity);
        }
    }
    for (uint32_t I = 0; I < S->Head->Figures; I++) P->Shown[I] = P->Laid[I];
    P->Fresh = 0;
}

#endif /* BOXOS_SCENE_PLAY_H */
