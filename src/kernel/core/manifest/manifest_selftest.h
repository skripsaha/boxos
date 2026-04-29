#ifndef MANIFEST_SELFTEST_H
#define MANIFEST_SELFTEST_H

#include "ktypes.h"
#include "error.h"

/*
 * In-kernel end-to-end test of the Manifest engine.
 *
 * Builds a tiny Manifest in a kernel buffer, registers a stub op into
 * OpRegistry, allocates Crates that point at kernel memory, executes, and
 * verifies the data flow. Runs at boot to prove that:
 *
 *   - OpRegistryInit / OpRegistryRegister / OpRegistryLookup work
 *   - ManifestSubsystemInit allocates and grows the slot table
 *   - ManifestCompile validates and indexes ops
 *   - ManifestExecute dispatches the right handler with the right Crates
 *   - ManifestRelease frees handles cleanly
 *
 * Side effects: registers two test op_kinds in OpRegistry that remain after
 * the test completes (harmless — they're in a reserved opcode range).
 */
error_t ManifestSelfTest(void);

#endif /* MANIFEST_SELFTEST_H */
