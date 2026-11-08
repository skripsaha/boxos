#ifndef BOX_LOAD_H
#define BOX_LOAD_H

#include "image_load.h"

/*
 * A box (box_format.h) put into a cabin, as image_load.h describes: read no
 * further than starting it needs, every part it reads held to its hash and the
 * table to the seal, its image a page at a time from the file into the pages
 * it will live in, at the base the kernel chooses when the box carries fixups
 * and where it says otherwise, with exactly the rights each part's kind gives.
 */
error_t BoxLoad(vmm_context_t *vmm, const ImageSource *source, const char *name, ImageLoaded *loaded);

#endif /* BOX_LOAD_H */
