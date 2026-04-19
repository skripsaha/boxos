---
name: Don't over-test in terminal
description: User prefers to test OS runs themselves; don't waste context on repeated QEMU runs
type: feedback
---

Don't run repeated QEMU tests to verify results. Run once to confirm compile + basic boot, then stop.

**Why:** Context window is expensive. The user has the machine and will test themselves. Repeated automated QEMU runs waste context without adding value.

**How to apply:** After implementing fixes, run `make clean && make` to confirm clean build. Run `make run` at most once to show it boots. Then deliver the summary and let the user test further themselves. If the user says "достаточно тестов" or "я протестирую сам" — stop immediately.
