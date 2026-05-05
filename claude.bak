- Use PascalCase primarily for functions, types, structs, and public interfaces.
- Use snake_case for variables.
- Use UPPER_CASE for constants.

- This is a bare-metal OS kernel project (BoxOS) written in C/assembly. When making fixes, always consider: lock ordering to prevent deadlocks, AMP/multicore safety, and that changes to one subsystem (e.g., keyboard, TagFS, VMM) can break others. Never apply a fix without reasoning about side effects on related subsystems.

- Use structured architecture, encapsulation, and explicit interfaces where they are helpful.
- Do not avoid language constructs just because they are rare; use them if they reduce complexity or improve correctness.
- Do not leave TODOs or placeholders unless the limitation is explicitly explained and tracked.

- When analyzing this codebase, read ALL relevant files before producing output. Do not start writing analysis after reading only a subset of files. If the user asks for a 'full' or 'thorough' analysis, use agents to explore the entire relevant subsystem first.

- Use make clean before full rebuilds or after build-system changes.
- Use make to build the project.
- Use make run to run the project.
- After each major phase of changes, rebuild and test in both modes:
  - make run
  - make run CORES=4 MEM=16G

- After applying any fix, always verify the build compiles cleanly (`make clean && make`) and run relevant tests before reporting success. If a fix touches locking or synchronization code, explicitly trace the lock acquisition order to check for deadlocks.

- Do not hardcode values when dynamic handling is possible.
- Keep files focused; prefer separate headers and clear module boundaries.
- Avoid extern unless there is a strong architectural reason.
- Avoid Unix-like assumptions and Unix philosophy unless they are explicitly justified.
- Write complete implementations, not stubs.

- Before starting work, create a todo list of all the steps needed. Check off items as you complete them so we can resume if interrupted.
