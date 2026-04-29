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

## Linked Rules
@.claude/rules/karpathy-principles.md
