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

## Advices
You are also can read claude's (AI) spaces, memory and notes for the context and understanding.



## Documentation
# Documentation Generation Instructions

## Output format
- Markdown (.md) only.
- No HTML, LaTeX, or PDF. Plain text with `##` and `###` headings only.
- One documentation file per module (e.g., `docs/render.md` for `src/render.cpp`).
- Do not add introductory phrases like "Here is the documentation...", "Sure!", etc. The file starts immediately with the `# <Module Name>` heading.
- Write all generated documentation in Russian. Keep code identifiers, type names, and file paths in English exactly as they appear in the source.

## General style (important)
- Write the way Arch Wiki writes: dry, precise, but explaining *why*, not just *what*.
- The short description of a function must use the imperative mood ("Draws", "Returns", "Flushes"). One sentence only.
- If a function has non-trivial logic (buffer handling, synchronization, OS constraints) — always add a "How it works" block.
- If a function is typically used together with others — add a "Typical usage" block.
- Do not invent behavior. If unsure, write "Requires verification in source code" instead of guessing.

## Template for each function/class

### `FunctionName`

**Short description:** one sentence, imperative mood.

**Parameters:**
- `param1` (type): description. Mark `in` / `out` / `in-out` when not obvious.
- `param2` (type): description.

**Returns:** (type) description. If `void` — write `void` and do not invent a value.

**Side effects:** state changes, console output, screen buffer modification, cursor movement, memory allocation, etc. If none — write "None".

**How it works:** (optional, but preferred for non-trivial functions) 2–5 sentences: algorithm, invariants, why it is implemented this way, which OS/hardware constraints apply.

**Notes:** (optional) limitations, thread safety, initialization requirements, known bugs, interaction with other modules...
If the source code contains important and accurate comments with meaningful clarifications — carry their essence into the "Notes" block of the documentation.

**Typical usage:** (optional) 1–3 sentences: in which scenario it is called, what it is combined with.

**Example:**
```cpp
// minimal compilable usage example


## Linked Rules
@memory-bank/karpathy-principles.md
