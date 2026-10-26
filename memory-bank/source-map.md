# Source map and authority order

## Authority order

1. The live Git tree, current branch, and actual verification output.
2. The tracked files in this `memory-bank/`, `GOALS.md`, and `PLANS.md`.
3. Preserved Claude memory and the v2 planning archive, used for history and
   decisions that are not visible in code.
4. Raw Claude JSONL and tool-result logs, used only for forensic questions.

## Preserved Claude sources

Project memory index:

`/Users/skripsaha/.claude/projects/-Users-skripsaha-Projects-boxos/memory/MEMORY.md`

Core BoxOS context:

- `memory/project_boxos.md`
- `memory/boxos_philosophy.md`
- `memory/boxos_production_constitution.md`
- `memory/boxos_master_table.md`
- `memory/feedback_v2_first.md`
- `memory/feedback_discuss_before_acting.md`
- `memory/feedback_tagfs_rebuild_what_we_remove.md`

Quill design and current v2 plan:

- `memory/tagfs_quill_design_2026_09_26.md`
- `memory/tagfs_wave1_map_2026_09_29.md`
- `memory/next_session_handoff_2026_10_14.md`
- `cc2f705e-330e-4946-8201-55b7a2b1dee3/v2/spec_v2.md`
- `cc2f705e-330e-4946-8201-55b7a2b1dee3/v2/s23/plan_final.md`
- `cc2f705e-330e-4946-8201-55b7a2b1dee3/v2/s23/NOTES.md`
- `cc2f705e-330e-4946-8201-55b7a2b1dee3/v2/s23/j2/NOTES.md`

The full session transcripts are the `*.jsonl` files beside the memory
directory. They are valuable historical evidence but must not be injected in
full into every task.
