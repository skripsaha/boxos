# Reusable Codex prompts

These prompts keep long sessions bounded while making the durable context
explicit.

## Orient

Read `AGENTS.md`, `GOALS.md`, `PLANS.md`, `memory-bank/activeContext.md`, and
the relevant `memory-bank` source map. Report the live branch, worktrees,
uncommitted changes, current phase, verified evidence, and open uncertainties.
Do not edit files.

## Prepare a phase

Read the applicable plan and source notes. Produce one bounded implementation
phase with exact files, invariants, lock-order impact, failure behavior, and
verification commands. Stop for approval when the phase introduces a new
public name or an architectural fork.

## Execute an approved phase

Read the phase sources completely, inspect the current implementation, then
implement the full phase. Preserve unrelated user changes. Check lock ordering,
AMP safety, subsystem side effects, and failure paths before running the
targeted verification.

## Handoff

Record only durable facts: date, branch or commit, files changed, tests and
their exact results, remaining uncertainty, and the next bounded phase. Update
`memory-bank/activeContext.md` and `memory-bank/progress.md`; do not paste a raw
transcript into the repository.
