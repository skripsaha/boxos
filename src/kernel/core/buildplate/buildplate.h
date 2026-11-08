#ifndef BOXOS_KERNEL_BUILDPLATE_H
#define BOXOS_KERNEL_BUILDPLATE_H

#include "ktypes.h"
#include "buildplate_format.h"

/* The plate this kernel carries. Read it; never write it — it is riveted by
 * tools/buildplate after the link, and the bytes here are what that tool
 * patched. */
const BuildPlate *BuildPlateRead(void);

/*
 * Say what this kernel was made from, in one or two lines.
 *
 * ‼ THE FIRST THING THE KERNEL SAYS. It runs after ScreenInit and BEFORE
 * serial_init, which is the only slot where the line is on the glass and in
 * the log ring for every boot — including the boots where there is no UART to
 * carry it. kprintf and never debug_printf: the whole point is that a shipped
 * build says it.
 */
void BuildPlateSay(void);

/* True when this image was packed while the harness had the tree mutated.
 * Anything that decides whether a measurement is evidence can ask. */
bool BuildPlateUnderTest(void);

#endif /* BOXOS_KERNEL_BUILDPLATE_H */
