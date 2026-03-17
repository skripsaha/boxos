# BoxOS v1.0 Release Plan

## Phase 0 — Critical Fixes (Showstoppers)
- [ ] 1. Fix VMM lock ordering (ctx->lock vs vmm_global_lock) → DEADLOCK
- [ ] 2. Fix OFE use-after-free (ref_count + write_lock) in TagFS
- [ ] 3. Fix meta_pool mirror race (don't release lock mid-write)
- [ ] 4. Fix idle_process_get() fallback → race condition
- [ ] 5. Fix executor.c buffer overflow (240-byte buf)
- [ ] 6. Fix journal replay ordering (mark before apply)

## Phase 1 — Code Hygiene (Uniformity)
- [ ] 7. Create linker_symbols.h, remove all extern from .c files
- [ ] 8. Move typedefs from .c files to *_types.h headers
- [ ] 9. Unified PAGE_SIZE define in one place
- [ ] 10. Unified error_t enum across codebase
- [ ] 11. Named constants instead of magic numbers (LAPIC, CPUID, PIT)
- [ ] 12. Remove redundant mfence() where lock already held

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
