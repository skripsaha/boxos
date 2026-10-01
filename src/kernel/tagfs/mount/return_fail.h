#ifndef TAGFS_MOUNT_RETURN_FAIL_H
#define TAGFS_MOUNT_RETURN_FAIL_H

#include "../tagfs.h"

/*
 * RETURNFAIL=on — the re-mount refused as a read the medium refused, and the
 * seat that then says it answers again (the note in return_fail.c). A
 * reproduction, not a feature: every call is nothing at all unless
 * `make RETURNFAIL=on`.
 *
 *   Tripped    the re-mount was just refused, on `seat`;
 *   SeatReady  a service pass: seat:ready said for that seat, once, when due.
 */
#if CONFIG_TAGFS_RETURN_FAIL_ONCE
void TagFSReturnFailTripped(uint8_t seat);
void TagFSReturnFailSeatReady(void);
#else
#define TagFSReturnFailSeatReady() ((void)0)
#endif

#endif /* TAGFS_MOUNT_RETURN_FAIL_H */
