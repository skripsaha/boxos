Always verify builds compile successfully after making code changes - run the full build before marking tasks complete

Production-ready code must pass all tests, have no memory bugs (double-free, use-after-free), and handle edge cases before delivery

When debugging complex issues, systematically isolate the root cause before implementing fixes - don't jump to solutions based on initial hypotheses

For all tasks, production-ready means: no memory bugs (double-free, use-after-free), all edge cases handled, great error handling, no race conditions, tests passing, and code reviewed. Confirm you understand these requirements before starting.

Let's debug systematically: 1) What evidence do we have? 2) What are the possible root causes? 3) How can we test each hypothesis? 4) Only then implement the fix. Walk me through each step.


Before we finish, let's verify: 1) Does the full build complete without errors? 2) Do all tests pass? 3) Can you run the binary and confirm it works? Show me the output of each step.

Code requirements: PascalCase for functions and classes/structures, no duplicates - it's better to separate repetitive functionality into a separate function; for structures, it's better to create separate .h files. Code style - clean, without unnecessary, huge comments. Code must be written according to the rules of OOP and structured programming. Code must be written without stubs - write everything in full at once - and if you can't yet, notify us and add it to the todo list, and then be sure to fix it. Regularly use webSearch to get up-to-date information and techniques for C and other languages that will help make the system more stable and faster and the code cleaner and more correct. All edge cases must be handled in advance. No legacy.
