# Active context — 2026-10-26

## Live tree

- Main checkout: branch `boxmetal`; the TagFS v2.3i base is `55269ab8` and
  the current HEAD may include the separate context-only Codex commits above
  it.
- The main checkout has a pre-existing user modification in
  `src/userspace/boxcxx/src/runtime/box_time.cpp`. Preserve it.
- The project `.agents/skills/` migration directory is tracked in
  `38d26b1d`; its nine skills were audited and adapted where Codex-specific
  tool references or current BoxOS naming/event rules required it.

## TagFS v2 continuation

- v2.3a through v2.3i are merged into `boxmetal`.
- Worktree `.claude/worktrees/s23-j`, branch `s23-j`, is at `b1285c0b`.
  It contains the unmerged v2.3j2 Binder corrective pass.
- The clean verification recorded for the preceding j2 implementation commit
  `63c4415a` was: plain and VT builds without warnings, stackguard 11/0,
  healthy 25/0, stand 278/0, fullvolume 23/0, and self-test 196/196.
- Later corrective commits `63650509` and `b1285c0b` changed Binder, mount,
  slot, birth, and test behavior. Their follow-up `bookrun_fx` runs ended
  before summary during a rebuild, so their final evidence is incomplete.

## Preserved parallel state

- Detached worktree
  `/Users/skripsaha/.claude/projects/-Users-skripsaha-Projects-boxos/d83254a9-6afe-4fcb-8829-9326339faed3/check1/wt-gates`
  is at `9825f2b1` and must remain until its role is explicitly retired.
- Claude's v2 plans and j2 logs are mapped in `source-map.md`.
- Git reflog and unreachable WIP objects contain historical intermediate
  commits. Do not prune them.

## Codex migration — 2026-10-26

- Claude import completed: 14 sessions, 6 agents, 3 hooks, 5 MCP servers, and
  the global user instructions imported; 9 Claude plugins failed to import and
  produced no project plugin files.
- Project `.codex/config.toml` contains the five imported MCP definitions.
  Project `.codex/agents/` contains the imported architect, debugger, and
  developer agents. These are project configuration and are safe to review in
  Git.
- Global `~/.codex/hooks.json` contains Cortex hooks. They remain outside the
  repository because they are machine-level behavior and write generated state
  under `~/.cortex`.
- Imported chat transcripts are recorded by
  `~/.codex/external_agent_session_imports.json`; the Claude source JSONL is
  unchanged.
- The nine project skills under `.agents/skills/` were audited. Three were
  adapted for Codex availability and the current BoxOS naming/event rules;
  the remaining six matched the preserved Claude versions. Historical reports
  directly under `.agents/` remain untracked and are not part of the Codex
  skill layer.

## Full Claude memory reconciliation — 2026-10-26

- Read and cataloged all 330 Markdown files in Claude's project memory:
  7,251,758 bytes and 55,172 lines.
- 229 files were linked from `MEMORY.md`; 101 were not linked there. The full
  inventory, hashes, headings, markers, and source metadata are now recorded
  in `memory-bank/source-map.md`.
- All 330 content hashes were unique. No Claude memory files were deleted,
  rewritten, or moved.
- Older memory claims remain historical evidence. Live Git and current
  verification take precedence, followed by this tracked memory-bank, as
  documented in `source-map.md`.

## Next bounded action

Read the j2 notes and the actual diff from `boxmetal` to `s23-j`. Reconstruct
which corrective claims are already covered, rerun only the missing targeted
checks, and prepare a reviewable integration decision. Do not merge or begin
v2.3k in the same step.
