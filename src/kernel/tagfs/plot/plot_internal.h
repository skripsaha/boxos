#ifndef TAGFS_PLOT_INTERNAL_H
#define TAGFS_PLOT_INTERNAL_H

#include "plot.h"

/*
 * Plot's own parts, shared by plot.c (the Plot and its runs), plot_write.c
 * (writing blocks and growing with zeros) and plot_gather.c (moving extents
 * together). Nothing outside plot/ calls these.
 */

/* [start, start + count) after runs[*n - 1], merged into it when it begins
 * where that one ends and cut into extents of at most PLOT_RUN_MOST blocks.
 * The array has room for count / PLOT_RUN_MOST + 2 more. */
void     PlotRunsPut(FileExtent *runs, uint32_t *n, uint32_t start, uint32_t count);

/* A run this call took, recorded as a Stake: OK or ERR_NO_MEMORY. */
error_t  PlotStake(Plot *p, uint32_t start, uint32_t count);

/* Ground a record named, come to rest in Fallow once a record that no longer
 * names it is inked (TagFSInkPlot): OK or ERR_NO_MEMORY. */
error_t  PlotLetGoLater(Plot *p, uint32_t start, uint32_t count);

/* How many extents the record has room for beside its tags and its name. */
uint32_t PlotRunRoom(const Plot *p);

/* Whether the record the Plot would ink fits format v1. */
bool     PlotRecordFits(const Plot *p);

/* A buffer of up to *most blocks from the heap, halved while the heap will not
 * give it — *most says how many it holds: NULL only when not one block fits. */
uint8_t *PlotRunBuffer(uint32_t *most);

#endif /* TAGFS_PLOT_INTERNAL_H */
