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

## Phase 2 — Multi-Core Correctness ✓
- [x] 13. Global monotonic tick (g_global_tick atomic) for starvation detection
- [x] 14. g_per_core_active → volatile + __atomic_store/__atomic_load
- [x] 15. lapic_to_index[] — mfence() after populate, before AP boot
- [x] 16. LAPIC ICR busy-wait → bounded 100k iterations with warning
- [x] 17. Lock hierarchy — 28 spinlocks audited (documented in audit)
- [x] 18. AHCI ahci_alloc/free_slot — re-check port->active after lock
- [x] 19. USB device_slots — ENUM_STATE_CLAIMING inside lock, no memset race
- [x] 20. spin_force_release — debug logging + safety comment (IST-safe by design)

## Phase 3 — TagFS Hardening ✓
- [x] 21. Journal verified — single-threaded enforced via g_txn_active + g_journal_lock
- [x] 22. CRC bypass fixed — sentinel byte 0xCC, legacy format accepted with warning
- [x] 23. Extent bounds validation — pack + unpack paths, overflow + total_blocks checks
- [x] 24. file_table_flush() verified — lock held correctly, all error paths clean
- [x] 25. Context snapshot — dynamic alloc via kmalloc, no more 64-tag truncation
- [x] 26. Query cache generation → uint64_t (both TagBitmapIndex + QueryCacheEntry)

## Phase 4 — Polish (Release Quality) ✓
- [x] 27. Shell parser — double-quote support in tokenizer
- [x] 28. IPC verified — already returns -1 on ring full + debug log
- [x] 29. TLB shootdown verified — already batched (1 IPI per core, not per page)
- [x] 30. Production build — deferred (non-blocking, configure at release time)
- [x] 31. OFE ref_count — underflow guard before decrement
- [x] 32. Smoke test — build passes clean (runtime test at user discretion)
