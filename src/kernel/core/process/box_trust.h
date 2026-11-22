#ifndef BOX_TRUST_H
#define BOX_TRUST_H

#include "ktypes.h"
#include "box_reader.h"

/*
 * A box's signature, checked (box_format.h, the part `signature`): whether it
 * holds — the author's key signed what the box is — whether the system trusts
 * that author, and the program's passport: who it is across every copy and
 * every version of it, its author and its name, so what is a program's own
 * stays its own when it is installed anew, and no one without the author's
 * key can be it.
 *
 * Anybody may sign with a key of their own; the system trusts by birth one
 * author, BoxOS — whoever built this kernel, whose public key is built into it
 * (build/include/box_author.h) — and a program it does not trust, or one that
 * carries no signature, still starts: as a guest, given what every program
 * has and nothing its box asks for, and never the system's.
 *
 * A passport has bit 63 set, so it is never a file's id (which a volume gives
 * from 1 up): the owner of a guest's own tags is its file, a signed program's
 * its passport.
 */
#define BOX_PASSPORT_BIT  (1ull << 63)

typedef struct {
    bool     Holds;      /* the signature is the author's over this box */
    bool     Trusted;    /* ... and the author is one this system trusts */
    uint64_t Passport;   /* the author and the name, BOX_PASSPORT_BIT set; 0 when it does not hold */
} BoxSigned;

/* The signature read from a box (BoxSignatureRead), checked against the
 * digest it is over (BoxSignedDigest). A how this kernel cannot check holds
 * nothing. */
BoxSigned BoxTrustCheck(const BoxSignature *signature, const uint8_t digest[BOX_HASH_BYTES]);

#endif
