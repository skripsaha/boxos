#ifndef TAGFS_INTEGRITY_INTERNAL_H
#define TAGFS_INTEGRITY_INTERNAL_H

#include "integrity.h"
#include "../tagfs.h"

/*
 * What the verify (integrity.c) and the rot a mount names (integrity_rot.c)
 * share, and nobody else.
 */

/* A mismatch IntegrityVerify found, named — said and published once a mount;
 * and a block no DIGESTS element names, named as rot is. Called with no TagFS
 * lock held. */
void     IntegrityRotName(uint64_t block, uint64_t stored, uint64_t read);
void     IntegrityUndigestedName(uint64_t block);

#endif /* TAGFS_INTEGRITY_INTERNAL_H */
