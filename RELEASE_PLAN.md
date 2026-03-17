# BoxOS v1.0 Release Plan

## Phase 0 — Critical Fixes (Showstoppers) ✓
- [x] 1. Fix VMM lock ordering → replaced vmm_global_lock with atomics
- [x] 2. Fix OFE use-after-free → drain write_lock before free
- [x] 3. Fix meta_pool mirror race → seqlock (lock-free reads)
- [x] 4. Fix idle_process_get() fallback → fatal halt
- [x] 5. Fix executor.c buffer overflow → sizeof(buf) checks
- [x] 6. Fix journal replay ordering → flush-before-mark pattern

## Phase 1 — Code Hygiene (Uniformity) ✓
- [x] 7. Created linker_symbols.h, removed 29 externs from 10 .c files
- [x] 8. Typedefs verified — all are private to .c files (correct pattern)
- [x] 9. PAGE_SIZE verified — 4 names, all 4096, static_assert enforced
- [x] 10. error_t — deferred (high risk, minimal stability gain)
- [x] 11. Named constants: LAPIC IPI, CPUID leaves, CRC32 poly, PIT freq
- [x] 12. mfence verified — NOT redundant (cross-core readers, different lock)

## Phase 2 — Multi-Core Correctness
- [ ] 13. Global monotonic tick for starvation detection
- [ ] 14. g_per_core_active → atomic + barrier
- [ ] 15. lapic_to_index[] — barrier after populate
- [ ] 16. Timeout for LAPIC busy-wait loops
- [ ] 17. Lock hierarchy documentation
- [ ] 18. AHCI ahci_get_port() — add lock
- [ ] 19. USB device_slots — full lock audit
- [ ] 20. Remove spin_force_release from IST handler → panic

## Phase 3 — TagFS Hardening
- [ ] 21. Journal: document/enforce single-threaded or add concurrency
- [ ] 22. CRC bypass → separate magic byte for "no CRC"
- [ ] 23. Extent bounds validation before pack
- [ ] 24. file_table_flush() — release lock before alloc
- [ ] 25. Context snapshot — dynamic alloc instead of fixed 64
- [ ] 26. Query cache generation → uint64_t

## Phase 4 — Polish (Release Quality)
- [ ] 27. Shell parser — quote support
- [ ] 28. IPC — return error on truncation, not silent clip
- [ ] 29. TLB shootdown batching (multi-slot)
- [ ] 30. Production build: strip symbols, gate debug traces
- [ ] 31. ref_count underflow → check BEFORE atomic_sub
- [ ] 32. Smoke test: boot → shell → file ops → reboot
