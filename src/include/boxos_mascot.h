/*
 * boxos_mascot.h — Duppo, the mascot of BoxOS, as its master draws it: discs
 * in the units of the master logo (duppogluten.svg; duppo-bubble.svg carries
 * the same numbers), y down. One account of its shape for every hand that
 * draws it — the utility duppo, in colour and turning, and Typecase's symbol,
 * in one colour.
 *
 * The colour logo is a body; the socket and the white of the eye in it; the
 * pupil, a hair taller than it is wide; three little bubbles — the medium one
 * on the body's lower right edge, the small one up to the left, the tiny one
 * to the right.
 *
 * The one-colour mark (the same file's black, pink and pale marks) is the
 * logo in two tones, laid one over another in this order: the body in the
 * ink, the socket in the paper, the white in the ink, the pupil in the paper
 * — a line of the paper round the eye, and the pupil the paper — then a disc
 * of the paper where the medium bubble stands over the body, the three
 * bubbles in the ink, the medium one a little nearer the body than in the
 * logo, and last a spark of the ink up to the left in the pupil.
 *
 * Plain numbers: the includer supplies nothing.
 */
#ifndef BOXOS_MASCOT_H
#define BOXOS_MASCOT_H

#ifdef __cplusplus
#define BOXOS_MASCOT_NUMBER static constexpr
#else
#define BOXOS_MASCOT_NUMBER static const
#endif

/* The body's pink, #E978A0: the colour Duppo is said in where it has one. */
#define BOXOS_DUPPO_PINK 0xE978A0u

typedef struct BoxDuppoDisc {
    float X, Y, Radius;
} BoxDuppoDisc;

typedef struct BoxDuppoOval {
    float X, Y, ReachX, ReachY;
} BoxDuppoOval;

BOXOS_MASCOT_NUMBER BoxDuppoDisc BoxDuppoBody   = {34.510509f, 28.268843f, 17.16255f};
BOXOS_MASCOT_NUMBER BoxDuppoDisc BoxDuppoSocket = {33.996025f, 25.892982f, 11.508257f};
BOXOS_MASCOT_NUMBER BoxDuppoDisc BoxDuppoWhite  = {33.996025f, 25.892982f, 10.013677f};
BOXOS_MASCOT_NUMBER BoxDuppoOval BoxDuppoPupil  = {33.771843f, 24.676687f, 7.8465376f, 7.9419012f};

/* The medium bubble, the small one and the tiny one: the order they are kept
 * in, so fewer bubbles drop the tiny one first. */
#define BOXOS_DUPPO_MOONS 3
BOXOS_MASCOT_NUMBER BoxDuppoDisc BoxDuppoMoons[BOXOS_DUPPO_MOONS] = {
    {45.021469f, 42.663223f, 5.1373868f},
    {16.983208f, 15.099807f, 3.5669854f},
    {55.582016f, 23.11713f, 2.4925005f},
};

/* What the one-colour mark has of its own: its medium bubble, the paper
 * round it, and the spark. */
BOXOS_MASCOT_NUMBER BoxDuppoDisc BoxDuppoMarkMoon  = {44.758939f, 42.531926f, 5.137387f};
BOXOS_MASCOT_NUMBER BoxDuppoDisc BoxDuppoMarkGap   = {44.561929f, 42.334982f, 5.990804f};
BOXOS_MASCOT_NUMBER BoxDuppoDisc BoxDuppoMarkSpark = {32.032647f, 22.148801f, 2.4925f};

#undef BOXOS_MASCOT_NUMBER

#endif /* BOXOS_MASCOT_H */
