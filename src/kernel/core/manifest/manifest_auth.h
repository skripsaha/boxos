#ifndef MANIFEST_AUTH_H
#define MANIFEST_AUTH_H

#include "ktypes.h"
#include "op_registry.h"

/*
 * Manifest op authorization — Phase 13 restoration of the kernel-side gate
 * that the prefix-chain dispatcher used to provide via system_security_gate.
 *
 * Each registered op declares an auth level via OpRegistration.security_mask
 * (re-purposed from a bitfield to an enum value). ManifestExecuteOnce calls
 * ManifestOpAuthorize before invoking each handler; on denial the op fails
 * with ERR_ACCESS_DENIED and the rest of the manifest follows the standard
 * OP_FLAG_OPTIONAL / OP_FLAG_SKIP_ON_ERROR semantics.
 *
 * Levels are ordered: SYSTEM > UTILITY > APP > NONE. A process with the
 * "system" tag passes any check; "utility" passes APP+UTILITY; "app" passes
 * APP only.
 *
 * Special tags:
 *   "god"     — passes everything regardless of level
 *   "stopped" — fails everything (process frozen)
 */

#define OP_AUTH_NONE     0u   /* anyone (kernel-internal manifests too) */
#define OP_AUTH_APP      1u   /* app | utility | system */
#define OP_AUTH_UTILITY  2u   /* utility | system */
#define OP_AUTH_SYSTEM   3u   /* system | bypass */
#define OP_AUTH_NETWORK  4u   /* network */

bool ManifestOpAuthorize(uint32_t op_kind, const OpContext *ctx);

#endif /* MANIFEST_AUTH_H */
