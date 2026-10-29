#ifndef BOX_CORE_IMAGE_SECTION_H
#define BOX_CORE_IMAGE_SECTION_H

#ifdef __cplusplus
extern "C" {
#endif

#include "box/defs.h"
#include "box/types.h"

/*
 * One named section of a program's image on the volume — .boxos.form,
 * .boxos.recipe — read at offsets: the ELF header, the section headers, the
 * name table and that one section, never the whole image.
 */

/* The section called Name in File. 1: found, *Bytes a heap copy with a NUL
 * after it (NULL when the section is empty), *Size its size. 0: the file is no
 * ELF64 image, or an image without that section. Negative: an image that
 * cannot be right or a section larger than Limit (-ERR_CORRUPTED), a read the
 * volume refused, no memory. *Image says whether the file is an ELF64 image;
 * it may be NULL. */
int image_section_read(file_id_t file, const char *name, size_t limit, char **bytes,
                       size_t *size, bool *image);

#ifdef __cplusplus
}
#endif

#endif /* BOX_CORE_IMAGE_SECTION_H */
