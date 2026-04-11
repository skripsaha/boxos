---
name: brainstorming
description: Use for architecture design, new OS subsystems, API decisions, trade-off analysis, and turning vague ideas into concrete plans.
---

# Brainstorming

## Purpose

Turn an idea into a precise, testable design with clear trade-offs.

## When to use

Use for:

- OS architecture
- new kernel subsystems
- API and ABI design
- scheduler and memory design
- filesystem and driver strategy
- feature planning
- design exploration

## Rules

- Do not jump directly to implementation.
- Always clarify goals, constraints, and non-goals.
- Produce multiple viable options.
- Compare options by simplicity, correctness, performance, and maintainability.
- Choose one recommendation and explain why.
- Keep the output concrete and technical.

## Workflow

1. Restate the goal.
2. Identify constraints and assumptions.
3. List the real problem to solve.
4. Generate 3 to 5 design options.
5. Compare trade-offs.
6. Pick the best option for the stated goals.
7. Define invariants and acceptance criteria.
8. Suggest the first implementation step.

## Output format

### Goal

-

### Constraints

- ...

### Options

- Option A: ...
- Option B: ...
- Option C: ...

### Trade-offs

- ...

### Recommendation

- ...

### Invariants

- ...

### Next step

- ...
