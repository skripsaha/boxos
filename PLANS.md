# Current plan

The canonical TagFS v2 plan remains preserved in the Claude session archive;
`memory-bank/source-map.md` gives the exact paths. This file is the compact
Codex entry point and records only the current phase and its dependencies.

## Current position — 2026-10-26

- `boxmetal` is at `55269ab8`, after v2.3i was merged.
- Worktree `s23-j` is at `b1285c0b`, an unmerged v2.3j2 corrective pass.
- The next safe action is to inspect and finish the j2 corrective pass, then
  run its targeted gates and two-lens review before any merge.
- v2.3k (final matrix and receipts) has not started. v2.4 follows only after
  the v2.3 completion evidence is clean.

## Phase discipline

1. Read the live tree, `memory-bank/activeContext.md`, and the relevant source
   notes.
2. Discuss or state the exact bounded phase and its non-goals.
3. Implement one complete phase in an isolated worktree when the change is
   large enough to risk the main branch.
4. Verify the changed path, the old format path, and the relevant BIOS/UEFI or
   multicore behavior before integration.
5. Update the handoff with commit names and evidence.

Do not merge `s23-j`, remove either preserved worktree, prune Git objects, or
start v2.4 while j2's interrupted corrective verification remains unresolved.
