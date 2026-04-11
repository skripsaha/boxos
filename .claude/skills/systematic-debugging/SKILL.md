---
name: systematic-debugging
description: Use for crashes, boot failures, regressions, memory corruption, UB, race conditions, and any unclear bug. Strict root-cause analysis only.
---

# Systematic Debugging

## Purpose
Find the real cause before proposing a fix. Do not jump to conclusions.

## When to use
Use for:
- crashes, panics, hangs, boot failures
- incorrect behavior
- regressions
- memory corruption
- undefined behavior
- race conditions
- flaky failures
- driver and kernel issues

## Rules
- Separate facts from hypotheses.
- Never invent evidence.
- Prefer the smallest test that can confirm or reject a hypothesis.
- If confidence is low, say so clearly.
- Do not propose a fix until the failure mechanism is understood.
- If information is missing, ask for the exact missing artifact.

## Workflow
1. Restate the symptom in one sentence.
2. List confirmed facts only.
3. List missing information.
4. Generate 2 to 5 hypotheses, ordered by likelihood.
5. Test the top hypothesis first.
6. Narrow down the failure mechanism.
7. Propose the smallest safe fix.
8. State what remains unverified.

## Output format
Use exactly this structure:

### Facts
- ...

### Hypotheses
- ...

### Tests
- ...

### Diagnosis
- ...

### Fix
- ...

### Risks
- ...

### Unverified
- ...
