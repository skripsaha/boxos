#ifndef EFI_SELFTEST_H
#define EFI_SELFTEST_H

/*
 * Boot-time self-test for the EFI crypto + ASN.1 + Authenticode
 * infrastructure.
 *
 * The verifier path is impossible to exercise end-to-end without a
 * real Secure-Boot-enrolled platform feeding us actual Microsoft-
 * signed PE images. But the building blocks (streaming SHA-256,
 * one-shot vs streaming consistency, bigint modular exponentiation
 * including RSA-PKCS1-v1.5 verify on real RFC test vectors, and DER
 * parser correctness) are all unit-testable in-kernel.
 *
 * Failing any of these at boot is a fatal misconfiguration — the
 * kernel will refuse to claim Secure Boot capability until the
 * problem is fixed.
 */

#include "ktypes.h"

/* Run every self-test. Returns true if all pass; false on any failure
 * (each individual failure is debug_printf'd at the same time). */
bool efi_selftest_run(void);

#endif /* EFI_SELFTEST_H */
