# Progress

## TagFS v2

- 2.1: FRAME format boundary and refusal path merged.
- 2.2 and 2.2b: width cleanup, u64 memory counts, and tag-id widening merged.
- 2.3a–g: format alphabet, mkfs/host reader, file parcels, mount reader, and
  derived block map merged.
- 2.3h: ribbon replay under glass, damage classification, record replay, Slip,
  and Gloss merged.
- 2.3i: kernel v2 writer for stitches, Docket/Jot, fresh-ground file changes,
  Fallow, claims, retry, and halt behavior merged.
- 2.3j1: fresh-ground data writes and file-record parcel splices are present in
  the s23-j line; its recorded verification covered v1 parity and 72 MiB
  readback.
- 2.3j2: Binder/Tally/Spine implementation and tests exist in s23-j. The final
  corrective pass is not yet integrated or fully re-verified.

## Evidence boundary

The entries above distinguish merged commits from worktree state. Historical
Claude reports may describe a planned or intermediate result; the live Git
branch and targeted verification output take precedence.

## Codex migration evidence — 2026-10-26

- Claude memory reconciliation read all 330 Markdown files under the preserved
  project memory root (7,251,758 bytes; 55,172 lines).
- The catalog is stored in `memory-bank/source-map.md`; it covers 229 files
  indexed by `MEMORY.md` and 101 files that were previously unindexed.
- SHA-256 content comparison found no duplicate files. The original Claude
  memory tree remains untouched.

## Next milestone

Finish and review j2, then complete the v2.3k matrix and receipts. Only after
that should the v2.4 one-flush/recovery work be planned from the preserved
specification.
