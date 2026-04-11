- Use PascalCase primarily for functions, types, structs, and public interfaces.
- Use snake_case for variables.
- Use UPPER_CASE for constants.

- Use structured architecture, encapsulation, and explicit interfaces where they are helpful.
- Do not avoid language constructs just because they are rare; use them if they reduce complexity or improve correctness.
- Do not leave TODOs or placeholders unless the limitation is explicitly explained and tracked.

- Use make clean before full rebuilds or after build-system changes.
- Use make to build the project.
- Use make run to run the project.
- After each major phase of changes, rebuild and test in both modes:
  - make run
  - make run CORES=4 MEM=16G

- Do not hardcode values when dynamic handling is possible.
- Keep files focused; prefer separate headers and clear module boundaries.
- Avoid extern unless there is a strong architectural reason.
- Avoid Unix-like assumptions and Unix philosophy unless they are explicitly justified.
- Write complete implementations, not stubs.