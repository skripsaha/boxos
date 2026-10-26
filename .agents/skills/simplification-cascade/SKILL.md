---
name: simplification-cascade
description: Use when the design is too complex, overengineered, hard to verify, or difficult to maintain. Reduces a problem to its simplest correct form.
---

# Simplification Cascade

## Purpose
Reduce complexity without breaking correctness.

## When to use
Use when:
- the design feels bloated
- the code has too many layers
- the implementation is hard to reason about
- the fix is getting too large
- the architecture is overabstracted
- there are too many special cases

## Rules
- Remove complexity before adding features.
- Prefer one simple mechanism over several interacting ones.
- Keep the behavior minimal but correct.
- Preserve essential invariants.
- Identify what can be deleted, merged, or delayed.

## Workflow
1. State the current complexity.
2. Identify the core requirement.
3. List everything that is not essential.
4. Remove unnecessary layers or indirection.
5. Collapse equivalent paths.
6. Reduce special cases.
7. Keep only the minimal correct design.
8. Verify the simplified version still satisfies the goal.

## Output format
### Core requirement
- ...

### Complexity sources
- ...

### Simplifications
- ...

### Minimal design
- ...

### What was removed
- ...

### Remaining risks
- ...
