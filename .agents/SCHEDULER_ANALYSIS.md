# Scheduler Critical Analysis Report

**Date:** 2026-04-03
**Reviewer:** Autonomous AI Agent
**Scope:** Full scheduler codebase (~1000 LOC)
**Verdict:** ⚠️ **NEEDS FIXES BEFORE PRODUCTION**

---

## 1. Architecture Overview

### Components
| File | Lines | Purpose |
|------|-------|---------|
| scheduler.c | 604 | Core scheduling logic, priorities, work stealing |
| runqueue.c | 137 | O(1) runqueue with 4 priority levels |
| context_switch.c | 152 | FPU save/restore, CR3 switching |
| idle.c | 122 | Per-core idle processes |
| ready_queue.c | 68 | Guide-ready process queue |
| kcore.c | 184 | K-Core pocket processing loop |

### Design Pattern
```
App Cores:                    K Cores:
  schedule()                    kcore_run_loop()
    ↓                             ↓
  scheduler_select_next()       kcore_queue_pop()
    ↓                             ↓
  context_restore_to_frame()    guide_process_one()
    ↓                             ↓
  iretq → userspace             execution_deck_handler()
```

---

## 2. Uniqueness Analysis

### What's UNIQUE (not in Linux/Windows/ZFS)

| Feature | BoxOS | Linux CFS | Windows | ZFS | Uniqueness |
|---------|-------|-----------|---------|-----|------------|
| **Tag-based scheduling** | ✅ | ❌ | ❌ | ❌ | **100% unique** |
| **Guide/Deck architecture** | ✅ | ❌ | ❌ | ❌ | **100% unique** |
| **Pocket/Result Ring IPC** | ✅ | pipes/sockets | LPC/ALPC | N/A | **Unique design** |
| **K-Core/App-Core split** | ✅ | SMP | NUMA | N/A | **Unique approach** |
| **Async syscall dispatch** | ✅ | sync only | sync only | N/A | **Unique** |
| **Work stealing** | ✅ | load_balance | load_balance | N/A | Similar concept |
| **O(1) scheduler** | ✅ | O(log n) | O(1) | N/A | Similar to O(1) Linux |
| **4 priority levels** | ✅ | CFS weights | 32 levels | N/A | Simpler than Windows |

### What's NOT unique
- O(1) runqueue (Linux 2.6 had this)
- Work stealing (common in multi-core schedulers)
- Context switch via interrupt frame (standard x86_64)
- Idle process with HLT (standard)

---

## 3. Critical Issues Found

### 🔴 CRITICAL: g_global_tick not incremented on all cores

**Location:** `idt/idt.c:454`
```c
// Only BSP increments g_global_tick via PIT IRQ 0
__atomic_fetch_add(&g_global_tick, 1, __ATOMIC_RELAXED);
```

**Problem:** App Cores use LAPIC timer for scheduling but `g_global_tick` is only incremented by BSP. This means:
- Starvation detection (`ticks_since_run`) is INACCURATE on App Cores
- `SCHEDULER_MILD_STARVATION_TICKS` (10 ticks) may never trigger on App Cores
- Fairness intervals are BSP-centric only

**Impact:** MEDIUM - Starvation detection works but with BSP timing, not per-core

---

### 🔴 CRITICAL: No preemption mechanism

**Location:** `scheduler.c:407-503`

**Problem:** `schedule()` is only called on:
1. LAPIC timer interrupt (every 10ms)
2. IPI_WAKE_VECTOR
3. Syscall (sync path only)

There is **NO preemption** - a process can run indefinitely until:
- It makes a syscall
- Timer interrupt fires
- It yields

**Impact:** HIGH - CPU-bound process can starve others for up to 10ms

---

### 🟡 MEDIUM: RunQueue capacity is fixed at 256

**Location:** `runqueue.h:13`
```c
#define RUNQUEUE_CAPACITY  256
```

**Problem:** If more than 256 processes are enqueued at same priority level, `runqueue_enqueue()` returns false with CRITICAL error.

**Impact:** LOW for now (system designed for <256 processes), but not scalable

---

### 🟡 MEDIUM: Work stealing cooldown is too aggressive

**Location:** `scheduler.h:23`
```c
#define STEAL_COOLDOWN_TICKS 3
```

**Problem:** 3 ticks = 30ms cooldown. In a 4-core system with uneven load, this may cause:
- Excessive stealing oscillation
- Cache line bouncing between cores

**Recommendation:** Increase to 10-20 ticks

---

### 🟡 MEDIUM: No load balancing on process creation

**Location:** `process.c:238-258`

**Problem:** Process `home_core` is assigned via round-robin on App Cores only. No consideration of:
- Current load on each core
- Process affinity requirements
- NUMA topology (if applicable)

**Impact:** MEDIUM - May cause uneven distribution

---

### 🟢 LOW: Use context lock contention

**Location:** `scheduler.c:83-152`

**Problem:** `scheduler_matches_use_context()` acquires `g_context_lock` on EVERY priority determination. This is called:
- On every enqueue
- On every schedule()
- On every re-enqueue of current process

**Impact:** LOW for now (single global lock), but will bottleneck on many cores

---

### 🟢 LOW: No scheduler statistics

**Problem:** No counters for:
- Context switches per second
- Average wait time
- Starvation events
- Work stealing success rate

**Impact:** LOW - Hard to debug performance issues

---

## 4. Code Quality Assessment

### Strengths
- ✅ O(1) process selection via bitmap
- ✅ Proper lock ordering documented
- ✅ Atomic operations used correctly for tag overflow
- ✅ Work stealing implementation is clean
- ✅ Context switch saves/restores all registers
- ✅ PCID support for TLB optimization
- ✅ FPU state management per process

### Weaknesses
- ⚠️ No preemption (only timer-based)
- ⚠️ g_global_tick not per-core
- ⚠️ Fixed capacity runqueue
- ⚠️ No scheduler statistics
- ⚠️ Use context lock is global bottleneck

---

## 5. Production Readiness Score

| Category | Score | Notes |
|----------|-------|-------|
| **Correctness** | 8/10 | Works but no preemption |
| **Performance** | 7/10 | O(1) selection, but lock contention |
| **Scalability** | 6/10 | Fixed capacities, global locks |
| **Reliability** | 8/10 | Proper error handling |
| **Maintainability** | 8/10 | Clean code, good comments |
| **Uniqueness** | 9/10 | Tag-based scheduling is novel |

### Overall: **7.7/10 - NEEDS FIXES BEFORE PRODUCTION**

---

## 6. Required Fixes

### Must Fix (Before Production)
1. **Add preemption** - Force reschedule after N consecutive runs
2. **Per-core tick counter** - Each core tracks its own ticks
3. **Increase STEAL_COOLDOWN_TICKS** - From 3 to 10

### Should Fix (For Better Performance)
4. **Dynamic runqueue capacity** - Or at least 1024
5. **Load-aware process placement** - Consider core load on creation
6. **Scheduler statistics** - Add counters for debugging

### Nice to Have (Future)
7. **NUMA awareness** - If hardware supports it
8. **Real-time priority** - For time-critical processes
9. **Cgroups-like limits** - Per-process CPU limits

---

## 7. Final Verdict

**The BoxOS scheduler is FUNCTIONALLY CORRECT and UNIQUE in its tag-based approach, but has production readiness gaps:**

1. **No preemption** - This is the biggest issue. A CPU-bound process can monopolize a core for up to 10ms.
2. **Global tick** - Starvation detection is BSP-centric, may not work correctly on App Cores.
3. **Fixed capacities** - Not scalable beyond designed limits.

**The Guide/Deck architecture is genuinely innovative** - no other OS uses tag-based process scheduling with pocket-based IPC. This is BoxOS's killer feature.

**Recommendation:** Fix the 3 "Must Fix" items, then the scheduler is production-ready.

---

**Signed:** Autonomous AI Agent
**Date:** 2026-04-03
