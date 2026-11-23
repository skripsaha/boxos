#ifndef TAGFS_VOLUME_LAY_FILES_H
#define TAGFS_VOLUME_LAY_FILES_H

#include "volume_lay.h"

/*
 * A laying's files and its book in memory (volume_lay_files.c), for
 * volume_lay.c alone.
 */

/* The first Tally's seq — every file's gen and every run's birth — and slot
 * 0's, as every maker lays them (§16.2 п.23). */
#define LAY_TALLY_SEQ  1u
#define LAY_SLOT_SEQ   2u

/* Every file the home volume keeps in a role, the kernel first, opened to be
 * read and taken as the new volume is to hold it — its tags and programs as
 * they are spoken there — and placed in a row from l->frame.data_block; room
 * for each block's digest. ERR_FILE_NOT_FOUND for a home volume that keeps no
 * kernel; ERR_TAGFS_NO_SPACE for files that run past the far Deed, or a
 * kernel a BOOT cannot count. Inside the home volume's door. */
error_t VolumeLayFilesTaken(VolumeLaying* l, TagFSVolume* home);

/* Every file let go — its handle closed — every tag, and the digests. */
void    VolumeLayFilesLetGo(VolumeLaying* l, TagFSVolume* home);

/* The book of the files as the digests stand: its leaves, and its plan from
 * the first grain past the files. ERR_TAGFS_NO_SPACE when it does not fit
 * before the far Deed, said; ERR_NO_MEMORY. */
error_t VolumeLayBookPlanned(VolumeLaying* l);

/* The Spine's counts of the book: past every file, tag and bookmark it names. */
SpineCounts VolumeLayCounts(const VolumeLaying* l);

#endif /* TAGFS_VOLUME_LAY_FILES_H */
