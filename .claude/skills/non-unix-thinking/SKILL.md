---
name: non-unix-thinking
description: Use when designing core OS concepts that must avoid default Unix assumptions and explore alternative paradigms.
---

# Non-Unix Thinking

## Purpose
Design OS concepts from first principles instead of defaulting to Unix-like assumptions.

## When to use
Use for:
- kernel architecture
- process model
- memory model
- IPC design
- filesystem semantics
- device model
- execution model
- privilege model

## Rules
- Do not assume processes, files, signals, pipes, POSIX semantics, or Unix conventions unless explicitly justified.
- Start from the actual goals of this OS.
- Derive abstractions from requirements, not from familiar defaults.
- If a Unix-like design is suggested, explain why it is necessary or reject it.
- Compare alternatives from first principles.
- Prefer the simplest model that matches the project’s goals.

## Workflow
1. Restate the OS goal without Unix assumptions.
2. List hidden Unix defaults that may be creeping in.
3. Replace each default with a requirement-driven alternative.
4. Generate multiple non-Unix design options.
5. Compare them against the project’s principles.
6. Choose the design that best matches the target paradigm.
7. State what must not be imported from Unix.

## Output format
### Goal
- ...

### Hidden Unix assumptions
- ...

### Alternative models
- ...

### Recommendation
- ...

### Forbidden imports
- ...
