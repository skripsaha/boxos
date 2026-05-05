# Andrej Karpathy Four Principles

## 1. Think Before Coding
- If unsure → ask. Do not assume
- If multiple interpretations → show them
- If unclear → stop, name it, ask

## 2. Simplicity First
- Minimum code that solves task. Nothing for "future"
- No abstractions for one-time code
- No flexibility/configurability not requested
- Senior engineer finds it over-engineered? → simplify
- 200 lines but 50 suffice → rewrite

## 3. Surgical Changes
- Change only what is requested
- Do not "improve" adjacent code
- Do not refactor what is not broken
- Every changed line must relate to my request

## 4. Goal-Driven Execution
- Not "add validation" → "write tests for invalid inputs, then make them pass"
- Not "fix bug" → "write test that reproduces it, then fix"
- Not "refactor X" → "ensure tests pass before and after"
