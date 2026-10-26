# BoxOS project memory

This is the repository's durable, reviewable context layer for Codex and any
other coding agent. Git and the live source tree remain authoritative for code;
these files preserve the reasons, constraints, phase state, and verified
handoffs that code alone cannot show.

Read `activeContext.md` first, then the one or two files relevant to the task.
Use `source-map.md` to reach older Claude notes when a historical detail is
needed. Keep raw Claude memory and JSONL archives read-only. Add a dated fact
only when it is supported by code, a test result, a user decision, or an exact
source path.

- `projectbrief.md` — project identity and architecture.
- `productContext.md` — the user's intended system and quality bar.
- `systemPatterns.md` — stable architectural rules and TagFS/Quill concepts.
- `techContext.md` — build, boot, hardware, and verification environment.
- `activeContext.md` — the current branch, worktrees, open risks, and next step.
- `progress.md` — milestone history with evidence.
- `source-map.md` — preserved Claude sources and their authority order.
