/*
 * boxos_mascot.h — Duppo, the mascot of BoxOS, as its master draws it: discs
 * in the units of the master logo (duppogluten.svg; duppo-bubble.svg carries
 * the same numbers), y down, and how each is shaded. One account of its shape
 * and its colour for every hand that draws it — the utility duppo, in colour
 * and turning, Typecase's symbol, in one colour, and the scene the machine
 * starts with (src/userspace/scenes/boot.txt, packed by tools/scene.py).
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

/*
 * How each disc of the colour logo is shaded (duppo-bubble.svg): a radial
 * gradient — the circle CX, CY, Radius of the gradient's own space, which the
 * matrix A B C D E F (SVG's matrix()) lays on the logo — through its stops,
 * each a place along the radius and the hue there, the first hue before the
 * first stop and the last past the last.
 */
typedef struct BoxDuppoShade {
    float A, B, C, D, E, F, CX, CY, Radius;
} BoxDuppoShade;

typedef struct BoxDuppoStop {
    float    At;
    unsigned Hue;   /* BOXOS_DUPPO_* below */
} BoxDuppoStop;

/* The hues: the body's pink and its deep plum, the lip round the eye, the
 * ring round the pupil and its shaded edge, the pupil — and the pupil's glow
 * and its rim, the pupil mixed straight with white by BOXOS_DUPPO_GLOW_SHARE
 * and BOXOS_DUPPO_RIM_SHARE of 255. BoxDuppoHues holds the first six as the
 * master has them; a hand that lets the person set the colours works them
 * from those. */
enum {
    BOXOS_DUPPO_LIGHT,
    BOXOS_DUPPO_DEEP,
    BOXOS_DUPPO_LIP,
    BOXOS_DUPPO_RING,
    BOXOS_DUPPO_RING_EDGE,
    BOXOS_DUPPO_CORE,
    BOXOS_DUPPO_GLOW,
    BOXOS_DUPPO_PUPIL_EDGE
};
#define BOXOS_DUPPO_GLOW_SHARE 0x3Bu
#define BOXOS_DUPPO_RIM_SHARE  0x29u
BOXOS_MASCOT_NUMBER unsigned BoxDuppoHues[6] = { BOXOS_DUPPO_PINK, 0x81265Cu, 0xE86696u, 0xFFE2F0u, 0x8B6077u, 0x000000u };

BOXOS_MASCOT_NUMBER BoxDuppoShade BoxDuppoBodyShade = {
    1.143646f, -0.09094687f, 0.09189171f, 1.1555275f, -124.59179f, -94.515235f, 128.0959f, 112.31412f, 16.922415f};
BOXOS_MASCOT_NUMBER BoxDuppoShade BoxDuppoSocketShade = {
    0.54407561f, 0.0f, 0.0f, 0.54407561f, -22.114865f, -36.849767f, 103.13065f, 115.31994f, 21.151943f};
BOXOS_MASCOT_NUMBER BoxDuppoShade BoxDuppoWhiteShade = {
    0.57626908f, -0.00182151f, 0.00186949f, 0.59144541f, -25.591621f, -42.04433f, 101.40121f, 113.55885f, 18.404936f};
BOXOS_MASCOT_NUMBER BoxDuppoShade BoxDuppoPupilShade = {
    1.1021106f, 0.92894668f, -1.0140942f, 1.2031302f, 7.747513f, -225.95277f, 124.25608f, 110.82202f, 7.3780317f};
BOXOS_MASCOT_NUMBER BoxDuppoShade BoxDuppoMoonShades[BOXOS_DUPPO_MOONS] = {
    {0.34233561f, -0.02722377f, 0.02750658f, 0.3458922f, -2.6037641f, 5.9093724f, 128.0959f, 112.31412f, 16.922415f},
    {0.23769016f, -0.01890198f, 0.01909834f, 0.24015957f, -16.083896f, -10.419087f, 128.0959f, 112.31412f, 16.922415f},
    {0.16609062f, -0.01320812f, 0.01334534f, 0.16781617f, 32.475734f, 5.2852998f, 128.0959f, 112.31412f, 16.922415f},
};

BOXOS_MASCOT_NUMBER BoxDuppoStop BoxDuppoBodyStops[2]   = {{0.51822966f, BOXOS_DUPPO_LIGHT}, {0.9999f, BOXOS_DUPPO_DEEP}};
BOXOS_MASCOT_NUMBER BoxDuppoStop BoxDuppoSocketStops[2] = {{0.87368202f, BOXOS_DUPPO_DEEP}, {1.0f, BOXOS_DUPPO_LIP}};
BOXOS_MASCOT_NUMBER BoxDuppoStop BoxDuppoWhiteStops[2]  = {{0.7325418f, BOXOS_DUPPO_RING},
                                                           {0.98360658f, BOXOS_DUPPO_RING_EDGE}};
BOXOS_MASCOT_NUMBER BoxDuppoStop BoxDuppoPupilStops[3]  = {
    {0.03465074f, BOXOS_DUPPO_GLOW}, {0.8121683f, BOXOS_DUPPO_CORE}, {0.9999f, BOXOS_DUPPO_PUPIL_EDGE}};
BOXOS_MASCOT_NUMBER BoxDuppoStop BoxDuppoMoonStops[2]   = {{0.0f, BOXOS_DUPPO_LIGHT}, {0.9297424f, BOXOS_DUPPO_DEEP}};

/* What the one-colour mark has of its own: its medium bubble, the paper
 * round it, and the spark. */
BOXOS_MASCOT_NUMBER BoxDuppoDisc BoxDuppoMarkMoon  = {44.758939f, 42.531926f, 5.137387f};
BOXOS_MASCOT_NUMBER BoxDuppoDisc BoxDuppoMarkGap   = {44.561929f, 42.334982f, 5.990804f};
BOXOS_MASCOT_NUMBER BoxDuppoDisc BoxDuppoMarkSpark = {32.032647f, 22.148801f, 2.4925f};

#undef BOXOS_MASCOT_NUMBER

#endif /* BOXOS_MASCOT_H */
