---
name: c-style
description: Use for writing and reviewing C code in the project’s required style, safety rules, and low-level conventions.
---

# C Style

## Purpose
Write C code that is consistent, readable, predictable, and safe for low-level systems work.

## When to use
Use for:
- kernel code
- runtime code
- boot code
- drivers
- memory management
- low-level libraries
- any C file in the project

## Rules
- Prefer clarity over cleverness.
- Keep functions small and single-purpose.
- Use explicit types and explicit ownership rules.
- Avoid hidden side effects.
- Handle errors immediately and consistently.
- Do not use undefined behavior.
- Do not rely on implicit conversions when they can hide bugs.
- Prefer simple control flow.
- Avoid excessive macros.
- Avoid nested complexity.

## Style rules
- Use consistent naming.
- Keep local scope narrow.
- Use `const` when data is read-only.
- Use `static` for internal helpers.
- Check all allocation and syscall failures.
- Return explicit error codes.
- Never ignore a failure path.
- Document invariants near the code that depends on them.
- Prefer straightforward structs and enums.
- Do not over-comment obvious code.

## Workflow
1. Understand the intent of the function.
2. Define ownership, lifetime, and error rules.
3. Write the simplest correct implementation.
4. Check for UB, leaks, races, and boundary errors.
5. Reduce complexity if possible.
6. Ensure the style matches the project.
7. Note any assumptions that need validation.

## Output format
### Intent
- ...

### Style checks
- ...

### Problems found
- ...

### Proposed code
- ...

### Notes
- ...
