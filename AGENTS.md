# BoxOS - Bare-metal OS Kernel (C/assembly)

## Build & Test
- `make clean && make` - full rebuild
- `make run` - single core
- `make run CORES=4 MEM=16G` - SMP test

## Critical Rules
- Lock ordering → check deadlocks before any fix
- AMP/multicore safety
- Subsystem side effects: keyboard, TagFS, VMM can break others

## Code Quality
- No patches. Implement full solution or say "I don't know"
- No garbage. Remove dead variables, huge comments, unused code
- No half-measures. 100+ line function → refactor. 1000+ line file → split
- Read ALL relevant files before analysis

## Working Mode
- Don't know → say "I don't know"
- Unsure → give 2-3 options with probability
- Decision → ask: "I propose X. Approve?"
- Missing info → ask me. Do not assume
- You propose → I decide
- New name → ask me
- One concrete task per session. Do not tackle everything at once
- No flattery. No "great question". Be direct

## MCP Priority
1. filesystem - read 3-5 files before advice
2. sequential-thinking - complex reasoning (5+ steps)
3. freebird/context7 - external knowledge
4. memory - save after my approval

## Persistent project context
- For every non-trivial or history-sensitive task, read `GOALS.md`, `PLANS.md`,
  `memory-bank/activeContext.md`, and the relevant memory-bank document before
  proposing work.
- Use `memory-bank/source-map.md` to locate the preserved Claude records. Read
  raw Claude transcripts only for a targeted historical question; they are not
  the live Git source of truth.
- Record durable decisions, verified results, and handoff state in
  `memory-bank/`. Include a date, commit or source path, and evidence when
  available. Do not write speculative conclusions as facts.
- Keep `AGENTS.md` short. Put architecture, history, and phase detail in the
  linked project files so the instructions remain discoverable without flooding
  every task's context.

## Linked Rules
@memory-bank/karpathy-principles.md
