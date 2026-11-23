#ifndef BOXOS_DECKS_H
#define BOXOS_DECKS_H

/* ============================================================================
 *  Deck identifiers — top byte of every (deck, op) syscall code.
 * ============================================================================ */
#define DECK_EXECUTION     0x00
#define DECK_OPERATIONS    0x01
#define DECK_STORAGE       0x02
#define DECK_HARDWARE      0x03
#define DECK_NETWORK       0x04
#define DECK_SYSTEM        0xFF

/* ============================================================================
 *  System deck opcodes — SINGLE SOURCE OF TRUTH for kernel handler tables
 *  and boxlib syscall wrappers.
 *
 *  NEVER mirror these defines in any *.c file. Add a new op here and include
 *  this header on both sides:
 *    kernel:    src/kernel/core/decks/system/system_deck.h includes it
 *    userspace: src/userspace/boxlib/src/X.c includes it
 *
 *  Sections are grouped by byte range so a new op is a one-line edit in the
 *  right block and can't collide silently with an existing range. Inside a
 *  section the numbers are dense: until the first release, an op that goes
 *  takes its number with it and the ops after it close up — every deck below
 *  keeps this rule.
 * ============================================================================ */

/* ─── Process + info (0x01 – 0x0D) ─────────────────────────────────── */
#define SYSTEM_OP_PROC_SPAWN        0x01
#define SYSTEM_OP_PROC_KILL         0x02
#define SYSTEM_OP_PROC_INFO         0x03
/* proc.crew — who wears a tag. params = the tag as text (no NUL).
 * out_crate = [u32 delivered][u32 total] then `delivered` records:
 * [u32 pid][u32 generation][u32 state][u32 tags_len][u64 cpu_us][char tags[]].
 * `total` is how many wear it, `delivered` how many fitted — the same
 * distinction storage.tag.query makes, for the same reason. broadcast has
 * always walked these carriers to SPEAK to them; this walk answers instead.
 * The caller is in its own answer when it wears the tag.
 *
 * Two promises about what is delivered: every record is a cabin that was LIVE
 * at the walk (PROC_DONE and PROC_CRASHED are dropped before the answer is
 * built, so a reader needs no liveness test of its own), and no record's tag
 * list is ever cut short — one that would not fit whole is left out of
 * `delivered` entirely and shows up as the shortfall against `total`. */
#define SYSTEM_OP_PROC_CREW         0x0B
/* Use Context — what the person at this machine is doing, said in tags.
 * One per machine (core/use_context). set takes a comma-separated tag list in
 * the in_crate (an empty list clears); get writes the tags into the out_crate
 * as [u32 count][u32 needed][(u16 len)(bytes)]*, needed being the byte length
 * of the whole list comma-joined with its NUL; clear takes nothing. set and clear need
 * system authority — the context is the user's, spoken for by the shell or a
 * system program — while get is open: any program may ask what the user is
 * doing. The volume remembers the context (a chain of its own the Ledger
 * names): set and clear answer, in an optional 1-byte out_crate, whether it
 * now does — 1 remembered, 0 not (no volume, no ground on it for the context,
 * a medium that will not take the write, a later word already said); the
 * context is set either way. change takes [u32 n][n bytes: the tags to
 * add][the rest: the tags to take out] — each a comma-separated list, either
 * empty — and changes the context in one word: added tags go after those it
 * says, a tag in both lists is taken out, nothing left clears; it needs system
 * authority and answers as set does. */
#define SYSTEM_OP_USE_SET           0x04
#define SYSTEM_OP_USE_GET           0x08
#define SYSTEM_OP_USE_CLEAR         0x09
#define SYSTEM_OP_USE_CHANGE        0x0D
/* The Touch tag the kernel says on that the Use Context says something else
 * than it did — set, changed, cleared, or taken up from a volume's memory —
 * after the change is made, so whoever hears it and asks get hears the new
 * context or a later one. Payload: one u64, the saying's number, counting
 * every word said since boot. */
#define USE_TOUCH_TAG               "use:changed"
/* () -> u64 microseconds of processor time used by the CALLING cabin. Self
 * only, deliberately: another cabin's processor time is information this op
 * has no business handing out, and a question about yourself needs no
 * authority to check. Backs std::clock(). */
#define SYSTEM_OP_PROC_CPUTIME      0x05
#define SYSTEM_OP_PROC_EXEC         0x06
/* proc.exec.file — proc.exec for one file, named by its id. params =
 * [u64 file id][caller-tag augment]; in_crate = the Luggage bytes alone (none:
 * no luggage); out_crate and auth as proc.exec. The file must be what a name
 * would have started — active, not deleted, a box or an ELF image by its first
 * bytes (no tag of the file is asked) — else ERR_FILE_NOT_FOUND. */
#define SYSTEM_OP_PROC_EXEC_FILE    0x0C
#define SYSTEM_OP_INFO              0x07
#define SYSTEM_OP_TLS_FSBASE        0x0A  /* set caller's FS base (its own thread pointer) */

/* ─── Memory (0x10) ─────────────────────────────────────────────────── */
/* Implicit huge-page prefault — boxlib memory.c calls this when growing
 * the user heap by >= 2 MB so the kernel pre-backs the new VA range with
 * 2 MB pages instead of waiting for 4 KB demand paging. The pre-fault
 * path falls back to 4 KB transparently if PMM cannot deliver a 2 MB
 * chunk. Params: [u64 va_base][u64 size]. */
#define SYSTEM_OP_HEAP_PREFAULT     0x10

/* ─── System telemetry (0x18 – 0x1D) ───────────────────────────────── */
#define SYSTEM_OP_DEFRAG_FILE       0x18
#define SYSTEM_OP_FRAG_SCORE        0x19
#define SYSTEM_OP_RESOURCES         0x1A
#define SYSTEM_OP_CORE_USAGE        0x1B
/* Every program aboard, each with its strands (roster_ops.c). params: none.
 * out_crate: [u32 delivered][u32 total] then `delivered` programs, each
 * [u32 pid][u32 generation] its main strand, [u32 pid][u32 generation] the
 * program it was started from (0 the system), [u64 program file][u64 began,
 * the uptime in us][u64 memory mapped for it, bytes][u32 times raised again]
 * [u32 strands][u32 tags_len][u32 0], then `strands` of [u32 pid]
 * [u32 generation][u32 state][u32 core][u64 processor time, us] — the main
 * strand first — then the tags, comma-joined, no NUL. A program live at the
 * walk; one that would not fit whole is the shortfall against `total`. */
#define SYSTEM_OP_ROSTER            0x1C
/* The machine's way from power-on to a shell, in moments (journey.c).
 * params: none. out_crate: a JourneyHead, then its `count` JourneyMoments —
 * the loader's first, then the kernel's (boxos_journey.h holds the layout).
 * The whole answer or ERR_BUFFER_TOO_SMALL; a few hundred bytes at most. */
#define SYSTEM_OP_JOURNEY           0x1D

/* ─── Process tags (0x20 – 0x22) ───────────────────────────────────── */
#define SYSTEM_OP_TAG_ADD           0x20
#define SYSTEM_OP_TAG_REMOVE        0x21
#define SYSTEM_OP_TAG_CHECK         0x22

/* ─── Route / broadcast / question (0x40 – 0x44) ──────────────────── */
#define SYSTEM_OP_ROUTE             0x40
#define SYSTEM_OP_ROUTE_TAG         0x41
#define SYSTEM_OP_ASK               0x42  /* (tag) in: question, out: answer — one answer, or the carrier's end */
#define SYSTEM_OP_ANSWER            0x43  /* (u32 asker pid)(u32 asker cookie) in: answer — the question's bytes go with it */
#define SYSTEM_OP_GIVE_BACK         0x44  /* (u64 addr)(u32 length): a message's bytes, read, given back */

/* ─── Diagnostics (0x50) ───────────────────────────────────────────── */
#define SYSTEM_OP_PERF_DUMP         0x50

/* ─── Touch handle-based ABI (0x51 – 0x58) ─────────────────────────── */
/* Userspace resolves a tag string ONCE via TOUCH_INTERN, caches the
 * uint32_t TouchTag locally, and passes it to all subsequent ops. The
 * hot syscall path takes the tag_id as a u32 in params — no string
 * parsing, no registry lookup. */
#define SYSTEM_OP_TOUCH_CLAIM       0x51
#define SYSTEM_OP_TOUCH_RELEASE     0x52
#define SYSTEM_OP_TOUCH_SEND        0x53
#define SYSTEM_OP_TOUCH_AWAIT       0x54
#define SYSTEM_OP_TOUCH_IRQ_RETURN  0x55
#define SYSTEM_OP_TOUCH_REGISTER    0x56
#define SYSTEM_OP_TOUCH_ACK         0x57
#define SYSTEM_OP_TOUCH_INTERN      0x58  /* tag string -> (full, bare) ids */

/* ─── EFI runtime introspection — read-only (0x60 – 0x62) ──────────── */
#define SYSTEM_OP_EFI_INFO          0x60
#define SYSTEM_OP_EFI_ESRT_GET      0x61
#define SYSTEM_OP_EFI_VERIFY_PE     0x62

/* ─── Bay — cross-cabin shared memory (0x70 – 0x72) ────────────────── */
/* open creates-or-attaches and returns a user-VA pointer; release drops
 * the per-cabin claim; size queries the region size. */
#define SYSTEM_OP_BAY_OPEN          0x70
#define SYSTEM_OP_BAY_RELEASE       0x71
#define SYSTEM_OP_BAY_SIZE          0x72

/* ─── Brook — SPSC ordered streaming (0x75 – 0x78) ─────────────────── */
/* open attaches with role + maps header/slots; release drops the claim
 * (last release destroys backing); handover moves the writer/reader seat
 * to a sibling strand of the SAME cabin, leaving the mapping where it is;
 * info snapshots live stats. */
#define SYSTEM_OP_BROOK_OPEN        0x75
#define SYSTEM_OP_BROOK_RELEASE     0x76
#define SYSTEM_OP_BROOK_HANDOVER    0x77  /* (u64 va_header)(u32 to_pid)   */
#define SYSTEM_OP_BROOK_INFO        0x78

/* ─── Manifest compile-and-reuse (0x80 – 0x81) ─────────────────────── */
/* Prepared-statement pattern for hot syscall paths: build a Manifest
 * once, COMPILE returns an 8-byte handle, submit many times via
 * POCKET_FLAG_MANIFEST_HANDLE skipping full validation. Handles are
 * per-process; auto-released at process_destroy. */
#define SYSTEM_OP_MANIFEST_COMPILE  0x80
#define SYSTEM_OP_MANIFEST_RELEASE  0x81

/* ─── Clipboard — the machine's list of what was copied (0x90 – 0x94) ── */
/* One list per machine, kept by the kernel (core/clipboard; the records and
 * the Touch it speaks in boxos_clipboard.h). Every op is open to any
 * program: the list is the person's, and what to put and what to take is the
 * program's to say.
 *
 *   put    params [u32 takes][u32 flags]; in_crate: the bytes, at least one;
 *          out_crate (optional): u64 the clip's number. takes is how many
 *          whole takes the clip stands, 0 for no such term; flags
 *          CLIPBOARD_WHILE_ALIVE. Memory that will not hold it is
 *          ERR_NO_MEMORY — nothing already in the list is let go for it.
 *   list   params [u64 from] — the newest clip numbered `from` or below, 0
 *          for the newest of all; out_crate: a ClipboardPage, then as many
 *          ClipboardRecords, newest first, as the crate holds whole.
 *   take   params [u64 number][u64 offset]; out_crate: the clip's bytes from
 *          `offset`, as many as the crate holds — its size says how many. A
 *          clip with takes left is taken only whole, from 0 into a crate
 *          that holds all of it, else ERR_BUFFER_TOO_SMALL and nothing is
 *          spent; a take that spends its last take ends the clip.
 *   drop   params [u64 number].
 *   clear  out_crate (optional): u64 how many clips went.
 *
 * A number no clip holds now — never given, or its clip gone — is
 * ERR_OBJECT_NOT_FOUND. */
#define SYSTEM_OP_CLIPBOARD_PUT     0x90
#define SYSTEM_OP_CLIPBOARD_LIST    0x91
#define SYSTEM_OP_CLIPBOARD_TAKE    0x92
#define SYSTEM_OP_CLIPBOARD_DROP    0x93
#define SYSTEM_OP_CLIPBOARD_CLEAR   0x94

/* ─── MemTag — RAM-region tagging (0xA0 – 0xAF) ────────────────────── */

/* Phase 1 — read-only surface. */
#define SYSTEM_OP_MEMTAG_QUERY      0xA0  /* required+any+excluded -> region_ids */
#define SYSTEM_OP_MEMTAG_INFO       0xA1  /* region_id -> {base, pages, tags...} */
#define SYSTEM_OP_MEMTAG_LOOKUP     0xA2  /* phys_addr -> region_id              */
#define SYSTEM_OP_MEMTAG_TAGS       0xA3  /* region_id -> list of tag strings    */
#define SYSTEM_OP_MEMTAG_STATS      0xA4  /* global MemTagStats snapshot         */

/* Phase 2A — enforcement infrastructure. SET_GUARD/GRANT/REVOKE gated on
 * the caller holding the "system" tag-bit (TagFS auth); CABIN_TAGS / CHECK
 * are unprivileged observation. */
#define SYSTEM_OP_MEMTAG_SET_GUARD  0xA5  /* tag_str + u8 on/off                */
#define SYSTEM_OP_MEMTAG_GRANT      0xA6  /* (u32 pid)(tag_str) -> grant cap    */
#define SYSTEM_OP_MEMTAG_REVOKE     0xA7  /* (u32 pid)(tag_str) -> revoke cap   */
#define SYSTEM_OP_MEMTAG_CABIN_TAGS 0xA8  /* u32 pid -> tag_str list            */
#define SYSTEM_OP_MEMTAG_CHECK      0xA9  /* (u32 pid)(u32 region_id) -> bool   */

/* Phase 2H+ — tag-driven PKU PTE stamping. APPLY_PKEY adds `pku:N` to the
 * region (replacing any prior pku:* tag), auto-stamping PTE bits 62:59
 * across every attach + cross-core TLB shootdown. Userspace WRPKRU then
 * gates per-key access. Unprivileged: callers may stamp any region they
 * know the region_id of — security comes from cabin's grant-set on the
 * region's other tags. */
#define SYSTEM_OP_MEMTAG_APPLY_PKEY 0xAA  /* (u32 region_id)(u8 pkey)           */
#define SYSTEM_OP_MEMTAG_LOOKUP_VIRT 0xAB /* (u64 virt) -> region_id (caller VM) */

/* ─── HW — real-HW per-process CPU state (0xB0 – 0xBF) ─────────────── */
/* Per-process state knobs that don't fit MemTag's region-centric model.
 * Each op acts on the CALLING process's VM context unless explicitly
 * params-targeted. Unprivileged: a process can read or set its OWN
 * state without elevation; cross-process mutation requires "system"
 * (not implemented in this opcode range yet). */
#define SYSTEM_OP_HW_LAM_GET        0xB0  /* () -> u8 lam_mode 0/1/2            */
#define SYSTEM_OP_HW_LAM_SET        0xB1  /* (u8 lam_mode) — 0=NONE,1=U48,2=U57 */
#define SYSTEM_OP_HW_TME_STATE      0xB2  /* () -> struct hw_tme_state          */

/* ─── Strand sync — park/wake on address (0xC0 – 0xC1) ─────────────── */
/* Substrate for std::atomic::wait / notify. Parked caller is resumed by
 * SysAddrWake or by the timeout armed via TouchQueueArmWake. */
#define SYSTEM_OP_ADDR_PARK         0xC0  /* (u64 va)(u64 expected)(u32 timeout_ms) */
#define SYSTEM_OP_ADDR_WAKE         0xC1  /* (u64 va)(u32 count — 0 = all)         */
#define SYSTEM_OP_STRAND_SPAWN      0xC2  /* (u64 entry_va)(u64 arg)[(u8 joinable)] -> out: u32 strand pid */
#define SYSTEM_OP_STRAND_RELEASE    0xC3  /* (u32 pid) — clear a joinable strand's reap-block so the reaper can reclaim it (std::thread join/detach); zombie-until-join keeps thread::id (=pid) unique while joinable */
#define SYSTEM_OP_STRAND_POOL_BIND  0xC4  /* (u64 pool_va)(u32 gen)(u64 orphan_pending_va) — bind this strand's StrandPool slab slot; kernel signals orphan_pending_va=1 after ORPHANED-stamp so userspace skips the 256-slot poll */

/* ─── Wait until a named incarnation is gone ────────────────────────── */
/* Being gone is a STATE, not an edge, and this op answers it as one: an
 * incarnation already finished is reported at once, a live one is parked on
 * and answered by the single place a life ends. There is no window between
 * asking and subscribing, so nothing can be missed — which is the whole
 * difference from watching the `process:died` multicast, where one dropped
 * announcement used to mean waiting forever. Generation is mandatory: a pid
 * alone names a seat, not a passenger, and the seat gets re-let. */
#define SYSTEM_OP_PROCESS_GONE      0xC5  /* (u32 pid)(u32 generation) -> Result.error_code=OK, data_length = (u32)int32 exit disposition (proc_exit.h); ERR_PROCESS_NOT_FOUND = never issued */

/* ─── Turn In — the strand that has nothing left to do lies down ─────── */
/* A strand does not wait on a RING; it waits for ANYTHING addressed to it.
 * This op says "I have looked past these two tails and found nothing — put me
 * down until one of them moves", and the delivery that moves a tail is the
 * same one that already flips the sleeper to PROC_WORKING. It answers nothing:
 * a reply would be a delivery, and a delivery wakes the strand the reply was
 * meant to put to sleep.
 *
 * A CURSOR, never "the ring is empty": between taking the mark and this op
 * running on a K-Core, an arrival can both land AND be drained by the caller's
 * own wait loop, and an emptiness test would then bed down a strand that has
 * the work in its hands. It carries no deadline, because there is no honest
 * one to guess — the caller loops and asks again. */
#define SYSTEM_OP_TURN_IN           0xC6  /* (u64 touch_tail_seen)(u64 result_tail_seen) — answers nothing */
/* A mark that watches no ring: the strand waits on the other one alone — a
 * Touch wait does not wake for good on answers and messages it is not taking.
 * Both unwatched is refused: that would be a sleep nothing can end. */
#define SYSTEM_TURN_IN_UNWATCHED    0xFFFFFFFFFFFFFFFFull

/* ─── Bell — wake a strand, and say nothing ──────────────────────────── */
/* Brook's two ends wake each other across a shared page the kernel never
 * sees, and a departing end has to be able to wake the survivor. All any of
 * them needs is for the sleeper's Result cursor to move; there is nothing to
 * say beyond "come".
 *
 * It is deliberately NOT an empty IPC message. That was tried, and it forced
 * every receive path in the system to learn that a contentless message is not
 * a message — a global change to what IPC means, made to hide one subsystem's
 * doorbell. A zero-length message is a perfectly legitimate thing for a
 * program to send, and swallowing it everywhere to serve Brook is the kind of
 * quiet semantic change that turns up months later as an unexplained lost
 * message. So the bell has its own op, and the record it pushes wears the
 * shape every consumer has discarded since long before it existed: no sender,
 * no token, ERR_WOULD_BLOCK — "this moved the cursor, that was its whole
 * job". */
#define SYSTEM_OP_BELL              0xC7  /* (u32 pid) — wake that strand; carries nothing */

/* strand.exit — the calling strand ends, and only it; its program goes on.
 * Every other end is the program's, all its strands at once: proc.kill names
 * another program (any strand of it) or, with target 0, the caller's own. */
#define SYSTEM_OP_STRAND_EXIT       0xC8  /* () — the calling strand ends; nothing comes back to it */

/* ─── Alarm — time, as an event ──────────────────────────────────────── */
/* A strand that must be woken at a moment cannot sleep on the moment: its
 * wait is for ANYTHING addressed to it (Turn In), and Turn In has no deadline
 * for the honest reason that a guessed one is a wake with nothing to do. So
 * the moment itself is made an arrival: at it, the kernel puts an ordinary
 * Touch — tag "alarm", an AlarmEvent carrying the moment it was set for — in
 * the TouchRing of the strand that set it, and that moves the cursor Turn In
 * already watches.
 *
 * One alarm per strand: setting again replaces the one pending, moving to 0
 * takes it back, and neither a replaced nor a cancelled alarm is delivered
 * once this op has run (an event already in the ring stays there, and says
 * which moment it was for). The moment is uptime in microseconds, the
 * ClockBoard's own, and it is kept to the microsecond — the touch queue's
 * keeper sets its one-shot for it — and a moment already past is delivered at
 * once. An alarm ends with its strand.
 *
 * It answers nothing, like Turn In and Bell: the arrival is the answer, and a
 * reply per frame to a strand that sets one per frame would be a delivery of
 * its own. There is nothing to refuse. */
#define SYSTEM_OP_ALARM             0xC9  /* (u64 moment_us; 0 = none) — answers nothing */

/* ============================================================================
 *  Storage deck opcodes — SINGLE SOURCE OF TRUTH, like the system deck above.
 *  Read by the kernel handler table (decks/storage/storage_ops.c) and by the
 *  boxlib wrappers (boxlib/src/file.c); never mirrored in a *.c file.
 * ============================================================================ */
/* tag.query — every file carrying the asked tags. params = [u8 scope][u8 whose]
 * (STORAGE_SCOPE_* and STORAGE_TAGS_* below) and after them the tags set aside,
 * in_crate = comma-separated tags of `whose` (absent = all).
 * out_crate = [u32 delivered][u32 total][u64 file_ids[delivered]]: `total` is
 * how many files match, `delivered` how many of them fit the crate — so a
 * caller with too small a crate learns the true count and can ask again with
 * room for it, instead of being handed the smaller number as the answer. */
#define STORAGE_TAG_QUERY           0x01
/* tag.set — params = [u64 file_id][u8 whose], in_crate = one tag, "key" or
 * "key:value", of `whose` (STORAGE_TAGS_* below): the file wears exactly that
 * tag, or the call is refused. tag.unset — the same params; in_crate "key"
 * takes off every one of `whose`'s tags of the key, "key:value" that one tag
 * alone — never another owner's of the same text. */
#define STORAGE_TAG_SET             0x02
#define STORAGE_TAG_UNSET           0x03
#define STORAGE_OBJ_READ            0x04
/* obj.write — params = [u64 file_id][u64 offset][u32 flags], and a ferry's
 * [u64 waybill] after them when it carries one; in_crate = the bytes; an
 * out_crate of 16 bytes = [u64 the bytes the file holds of the write][u64 the
 * size it left], on every answer. STORAGE_WRITE_APPEND: the write begins at
 * the file's size as it is when the write is let at the file — `offset` is not
 * read — so writers appending to one file never lay their bytes over each
 * other's, and each learns where its own begin: the size it left less what it
 * wrote. */
#define STORAGE_OBJ_WRITE           0x05
#define STORAGE_WRITE_APPEND        (1u << 0)
#define STORAGE_OBJ_CREATE          0x06
#define STORAGE_OBJ_DELETE          0x07
#define STORAGE_OBJ_RENAME          0x08
/* obj.getinfo — params = [u64 fid][whose]; out_crate = [u64 fid][u64 size]
 * [u32 tags][u16 name_len][name][tag records]…[u64 kept_until][u64 made_by]:
 * the tags are those of `whose` the file wears (STORAGE_TAGS_* below) —
 * STORAGE_TAGS_ALL every one its caller may see —, each record [u16 key_len]
 * [u16 value_len][u8 1 for a tag the system keeps, else 0][u64 owner][key]
 * [value], owner 0 the person's and a program's a file id of it — and
 * after them [u64 kept_until][u64 made_by][u8 role][u8 head_len][head]: the
 * eight
 * bytes before the last are the time, in nanoseconds of the wall clock, at
 * which the volume lets a deleted file go (obj.keep), 0 for a file that is not
 * deleted; the last eight are the volume's id of the file of the program that
 * made this file, 0 when no program of the volume did (the system's own, or a
 * program started from bytes) — the program's file no promise of a file still
 * there: it may have been deleted since; then what the system keeps the file
 * as, 0 an ordinary file, 1 a system file, 2 a boot file, 3 the kernel; and
 * the file's first bytes, as storage.head.query asks them, head_len of them
 * (0 to STORAGE_HEAD_MOST). */
#define STORAGE_OBJ_GET_INFO        0x09
/* obj.span — a file made exactly `size` bytes long. params =
 * [u64 file_id][u64 size][u32 flags], the flags word optional (0). Shorter:
 * the tail is cut and its blocks given back. Longer: the bytes from the old
 * size to the new one read as zeros, written to blocks taken for them — never
 * a previous owner's bytes. A write grows a file only at its end, and is
 * refused ERR_TAGFS_BEYOND_SIZE past it; span is the one other way a file
 * grows. STORAGE_SPAN_GROW: the file is grown to `size` only if it is shorter,
 * and left as it is otherwise — decided where nothing else changes the file
 * meanwhile, so a writer that spans to where its write begins never cuts back
 * what another writer added since it looked. */
#define STORAGE_OBJ_SPAN            0x0A
#define STORAGE_SPAN_GROW           (1u << 0)
/* anchor — durability + tag-event fan-out. params = [u64 file_id].
 * Answered once every change made before it is settled and lies in a block
 * the volume never writes again; the caller waits behind its Chit, never
 * inside the kernel. file_id == 0 publishes a generic "anchor" Touch event.
 * Non-zero additionally fans out the event on every tag of that file and on
 * the words its name answers to (the whole name, and its stem — the name is no
 * tag, but a program is known to those who watch its files by it), so
 * observers keyed by either (Touch REST) wake with payload {file_id, op=2, ...}.
 * No POSIX equivalent — fsync() returns silently and offers no notification
 * side-channel. */
#define STORAGE_OBJ_ANCHOR          0x20
/* obj.write.settled — the wait for a file's changes to reach the medium on
 * their own: params = [u64 file_id]; 0 = every change of the volume so far.
 * Answered, with nothing published, once the file's newest change is settled
 * and lies in a block the volume never writes again — or at once, when it is.
 * A demand: the cycle it waits for does not wait for Lull. The medium's
 * lasting refusal answers with its code (ERR_TAGFS_READ_ONLY,
 * ERR_DEVICE_NOT_READY). */
#define STORAGE_OBJ_WRITE_SETTLED   0x21
/* obj.escheat — Escheat, the act that lets go what a book can never give
 * back: every rotten range the Tally stood on
 * names — its files declared lost, the blocks held for it let go, a rotten
 * DIGESTS range's digests taken anew of the data the living files name — and
 * every tail a Slip holds that can never be laid in its block, the file left
 * with its blocks alone. The next Tally names none of it. params = none.
 * out_crate = [u64 ranges][u64 files lost][u64 blocks let go][u64 tails lost]
 * [u64 DIGESTS ranges known rotten, laid anew]. Only a cabin wearing system or god: any other is
 * refused (ERR_ACCESS_DENIED at the gate, ERR_PERMISSION_DENIED past it).
 * ERR_OBJECT_NOT_FOUND when there is nothing to let go. */
#define STORAGE_OBJ_ESCHEAT         0x22
/* tag.list — the tags of `whose` files wear, each with how many files wear it:
 * every volume of the house in the order of its number, each's tags in the
 * order of its numbers for them — a tag worn on two volumes is listed by each,
 * with the files of that volume. A volume's registry names more than files'
 * tags — a process's, a Touch's, a Bay's, another owner's — and a tag no file
 * wears is left out. params = [u64 from][u8 scope][u8 whose][tags set aside]:
 * from the volume numbered from's high 32 bits and its tag numbered by the
 * low 32 on (0: the first), the files counted inside the Use Context
 * (STORAGE_SCOPE_USE, the files that wear its tags) or on the whole volume,
 * those wearing a tag the comma list after the scope names left uncounted, as
 * a query leaves them out. With STORAGE_TAGS_ALL — every tag its caller may
 * see, each with whose it is — the scope is every volume's whole and no tag
 * is set aside. out_crate = [u32 delivered][u32 0][u64 next], then
 * `delivered` records [u32 files][u8 flags][u8 key_len][u16 value_len]
 * [u64 owner][key][value] while they fit, owner as obj.getinfo says; `next`
 * is the number to ask from for the rest, 0 when there are none. A crate too
 * small for even the first record is refused ERR_BUFFER_TOO_SMALL. */
#define STORAGE_TAG_LIST            0x23
#define STORAGE_TAG_LIST_HEAD       16u     /* [u32 delivered][u32 0][u64 next] */
#define STORAGE_TAG_LISTED_HEAD     16u     /* a record before its text */
#define STORAGE_TAG_LISTED_SYSTEM   0x01u   /* flags: a tag the system keeps */
/* obj.conjugate — Conjugate, a duplicate laid on the blocks it duplicates:
 * `count` blocks of file `fid` from its block
 * `at`, each held byte for byte to the block of file `onto` from its block
 * `onto_at` it is laid on, named by a run SHARED on that file's ground — its
 * own let go, its bytes, size and times as they were. Every block alike or
 * none laid. params = [u64 fid][u64 at][u64 onto][u64 onto_at][u64 count].
 * out_crate = [u64 blocks laid][u64 the block of `fid` a refusal met][u64 why]:
 * why 0 laid, 1 the cabin wears neither system nor god, 2 a file the system
 * keeps or the kernel's, 3 the blocks asked are not `fid`'s plain ground or
 * not `onto`'s, 4 a block with no digest the book holds, 5 the digests differ,
 * 6 the digests agree and the bytes differ, 7 a file changed meanwhile, 8 the
 * volume's own refusal — the error says it. Only
 * a cabin wearing system or god (ERR_ACCESS_DENIED at the gate,
 * ERR_PERMISSION_DENIED past it). */
#define STORAGE_OBJ_CONJUGATE       0x24
/* obj.keep — a file deleted for a term: params = [u64 fid][u64 term seconds].
 * No query meets the file from now on, it can be brought back by obj.restore
 * until the term ends, and the volume lets it go by itself then (tagfs/keep/
 * keep.h). The term is the caller's — a program passes what its person
 * chose — and a term of 0 is the file given up at once, as obj.delete does.
 * Guarded as obj.delete is, by the same one guard and the same warrant (a
 * file the system keeps is refused, ERR_PERMISSION_DENIED). Deleting a
 * deleted file moves its term. */
#define STORAGE_OBJ_KEEP            0x25
/* obj.restore — a deleted file brought back: params = [u64 fid]. The file is
 * found by queries again, its tags and name as they were. ERR_FILE_NOT_FOUND
 * when the volume holds no such file or its term has ended; OK, changing
 * nothing, for a file that is not deleted. */
#define STORAGE_OBJ_RESTORE         0x26

/* name.query — the files called a word. A file's name is a field of its card,
 * not a tag, and this is the one question that finds files by it: the word
 * matches a file when it is the whole of the file's name or its stem (the name
 * up to its last dot, so `slot3` finds `slot3.sav`), byte for byte; a file
 * kept deleted (obj.keep) is not met. params = [u8 scope][u8 whose][tags set
 * aside], as tag.query's — a name is no one's, `whose` the owner of the tags
 * asked beside it: inside the Use Context (STORAGE_SCOPE_USE) the files
 * called the word that wear the context's tags too, or STORAGE_SCOPE_
 * EVERYWHERE; STORAGE_SCOPE_DELETED is refused; after the scope, a comma list
 * of tags no file of the answer wears. in_crate = the word, its bytes whole
 * (no tag spelling: a ':' or ',' is a byte like any other), and — after one
 * NUL — a comma list of `whose`'s tags the files called so must wear too; an empty word
 * is refused ERR_INVALID_ARGUMENT.
 * out_crate = [u32 delivered][u32 total][u64 file_ids[delivered]] in file id
 * order, as tag.query's. */
#define STORAGE_NAME_QUERY          0x27

/* tag.show — a program's own tags shown to another program: params = [u8 act]
 * [u64 program]. STORAGE_SHOW_TO puts the program — a program's file, or 0
 * for every program, those to come too — in the list the caller's program
 * shows all its tags to, STORAGE_HIDE_FROM takes it out; one shown them sees
 * them on files, finds files by them and lists them (STORAGE_TAGS_PROGRAM),
 * and never puts them on nor takes them off. STORAGE_ASK_TO_SEE asks the
 * program to show the caller its tags: a note the kernel holds until the
 * program shows them to the caller or hides them from it, either leaves, or
 * the volume is mounted again — and when it leaves a note, the kernel says
 * so on the Touch word "tags-asked" once for every file of the program asked,
 * its payload the [u64] file id, and nothing of who asked. The list is the
 * program's, kept on the cards of its files — a signed program's on every
 * file of it: a program started from bytes, or none of whose files is on the
 * volume, has none — ERR_TAGFS_NO_OWN_TAGS. A program
 * the volume has not: ERR_FILE_NOT_FOUND; one shown or asked to show to
 * itself, or asked as 0: ERR_INVALID_ARGUMENT. */
#define STORAGE_TAG_SHOW            0x28
#define STORAGE_SHOW_TO             0
#define STORAGE_HIDE_FROM           1
#define STORAGE_ASK_TO_SEE          2

/* tag.shown — the lists of tag.show: params = [u8 which][u64 program][u64 from].
 * STORAGE_SHOWN_TO_ME: the programs that show their tags to the caller's, by
 * name or to every program. STORAGE_SHOWN_BY: the programs `program` shows its
 * tags to, 0 first for every program — asked by the program itself or by one
 * whose box asks `see-tags` or `change-tags` (ERR_ACCESS_DENIED otherwise).
 * STORAGE_ASKS_TO_SEE: the programs that asked the caller's to show them its
 * tags and are not answered. `program` is read for SHOWN_BY alone. Each in
 * file id order from `from` on (1, or 0, for the first). out_crate = [u32
 * delivered][u32 total][u64 ids[delivered]], as storage.query's. A caller
 * with no program of its own (tag.show) asking of its own: ERR_TAGFS_NO_OWN_TAGS. */
#define STORAGE_TAG_SHOWN           0x29

/* head.query — the files that begin with given bytes. What a file is is told
 * by its first bytes, by whoever knows what they mean — a box's, a font's, an
 * itinerary's — and every file's card keeps them, STORAGE_HEAD_MOST at the
 * most, exact through every change of what the file holds: no file is read,
 * and the question asks nothing of anybody, as a file's name and size are
 * anybody's to see. in_crate = the bytes, 1 to STORAGE_HEAD_MOST of them
 * (ERR_INVALID_ARGUMENT otherwise); no params. out_crate = [u32 delivered]
 * [u32 total][u64 file_ids[delivered]] in file id order, as storage.query's;
 * a file kept deleted (obj.keep) is not met. */
#define STORAGE_HEAD_QUERY          0x2A
#define STORAGE_HEAD_MOST           16u

/* volume.list — every volume of the house: the home volume, the one the
 * machine was booted from, number 0, and every other one met on any medium
 * since, each numbered as it was first met and never numbered again this
 * boot. A file's id names its volume: the number in bits 48..62 of it
 * (STORAGE_VOLUME_FILE_SHIFT). No params. out_crate = [u32 delivered]
 * [u32 total], then `delivered` records [u32 number][u32 flags (STORAGE_
 * VOLUME_*)][u8 uuid[16]][u64 bytes][u64 free bytes][u64 files][u16 name_len]
 * [u16 medium_len][name][medium] while they fit, in the order of the numbers:
 * the size, the free bytes and the files of a volume that is mounted, 0 of one
 * that is not; `name` the one a person gave the volume, none when nobody
 * has; `medium` what the medium it is on calls itself. */
#define STORAGE_VOLUME_LIST         0x2B
/* volume.info — one volume of the house: params = [u32 number]; out_crate =
 * one record as volume.list's. ERR_OBJECT_NOT_FOUND for a number the house
 * gave no volume. */
#define STORAGE_VOLUME_INFO         0x2C
/* volume.name — the name a person gives a volume, by the person's hands
 * (change-tags; ERR_ACCESS_DENIED otherwise): params = [u32 number][u16 len]
 * [name], `len` bytes of UTF-8 of any language, at most STORAGE_VOLUME_NAME_
 * MOST, no control character — 0 takes the name away. Kept in the volume's
 * book, the name goes with the volume to every machine it is met on. The
 * answer comes once the volume holds it: OK; ERR_INVALID_ARGUMENT for one not
 * spelled as a name; ERR_OBJECT_NOT_FOUND for a number the house gave no
 * volume; ERR_DEVICE_NOT_READY for a volume whose medium left;
 * ERR_TAGFS_READ_ONLY for one that takes no change; ERR_TAGFS_NO_SPACE for a
 * name its book had no room for, the old name kept. */
#define STORAGE_VOLUME_NAME         0x2D
#define STORAGE_VOLUME_NAME_MOST    255u    /* volume_tally.h TALLY_VOLUME_NAME_MOST */
#define STORAGE_VOLUME_LISTED_HEAD  52u     /* a record before its name */
#define STORAGE_VOLUME_HOME         0x01u   /* flags: the volume the machine was booted from */
#define STORAGE_VOLUME_MOUNTED      0x02u   /* its book is read and its files are found */
#define STORAGE_VOLUME_READ_ONLY    0x04u   /* it takes no change: under glass, or its medium refuses writes */
#define STORAGE_VOLUME_AWAY         0x08u   /* its medium left; its book is held open for its return */
#define STORAGE_VOLUME_REMOVABLE    0x10u   /* its medium is a removable one */
#define STORAGE_VOLUME_FILE_SHIFT   48u     /* a file id's volume number begins at this bit */
#define STORAGE_SHOWN_TO_ME         0
#define STORAGE_SHOWN_BY            1
#define STORAGE_ASKS_TO_SEE         2

/* The scope byte storage.query, storage.name.query, storage.tag.list and
 * storage.create carry in their params.
 *
 * A question in the person's tags is asked, and a file the person's tags are
 * put on is created, INSIDE the Use Context: the user said `use code cpp`, so
 * "project,year:2026" means code AND cpp AND project AND year:2026, and a new
 * file is stamped code and cpp along with whatever tags it was given.
 * EVERYWHERE asks the same thing of the whole volume, the context left out.
 * The Use Context is the person's lens: a call in a program's own tags is
 * asked of the whole volume whatever its scope says. query: params =
 * [u8 scope][u8 whose]. create: params = [char filename[32]][u8 scope][u8 whose].
 *
 * DELETED asks storage.query for the files kept deleted (obj.keep) alone —
 * the whole volume's, the Use Context left out, and only those: the one
 * explicit question that sees them. It is no scope for storage.create,
 * storage.name.query or storage.tag.list. */
#define STORAGE_SCOPE_USE           0
#define STORAGE_SCOPE_EVERYWHERE    1
#define STORAGE_SCOPE_DELETED       2

/* The whose byte every call that speaks tags carries, after the bytes its
 * params lay out before it (storage.query, storage.name.query, storage.tag.
 * list, storage.tag.set, storage.tag.unset, storage.create, obj.getinfo) —
 * with STORAGE_TAGS_PROGRAM, the [u64] file id of the program right after it
 * — the program that file holds; a signed program is one program in every
 * file of it —,
 * and the params that follow the whose after that:
 *
 *   STORAGE_TAGS_MINE    the program's own tags — its words for its own files,
 *                        which no other program meets, and which it needs no
 *                        asking for — a signed program's, in every file of it.
 *                        A program started from bytes held in memory, or none
 *                        of whose files is on the volume any longer, has
 *                        none: ERR_TAGFS_NO_OWN_TAGS.
 *   STORAGE_TAGS_PERSON  the person's tags — the labels the person finds
 *                        files by, and the Use Context's words: seen only by a
 *                        program whose box asks `see-tags` or `change-tags`,
 *                        put on and taken off only by one that asks
 *                        `change-tags` (ERR_ACCESS_DENIED otherwise).
 *   STORAGE_TAGS_PROGRAM another program's own tags, the program named by a
 *                        file of it: seen by one it shows them to (storage.tag.show)
 *                        and by one whose box asks `see-tags` or `change-tags`
 *                        — the person sees every mark on his files —, taken
 *                        off a file only by one that asks `change-tags` — the
 *                        person's hands —, and put on by none but the program
 *                        itself (ERR_ACCESS_DENIED otherwise). The caller's
 *                        own program named so is STORAGE_TAGS_MINE.
 *   STORAGE_TAGS_ALL     every tag its caller may see, each with whose it is:
 *                        its own, those of the programs that show it theirs,
 *                        and with `see-tags` or `change-tags` the person's and
 *                        every program's — for obj.getinfo and storage.tag.
 *                        list alone; a question, a create or a change is of
 *                        one owner's tags (ERR_INVALID_ARGUMENT).
 *
 * Absent, program 0, or any other value: ERR_INVALID_ARGUMENT. */
#define STORAGE_TAGS_MINE           0
#define STORAGE_TAGS_PERSON         1
#define STORAGE_TAGS_PROGRAM        2
#define STORAGE_TAGS_ALL            3

/* ============================================================================
 *  Hardware deck opcodes — the same single source, for the kernel's handler
 *  table (hardware_ops.c) and boxlib's wrappers alike. What the deck no longer
 *  offers, and why, hardware_ops.c says above its Disk and USB sections.
 * ============================================================================ */
#define HW_TIMER_GET_TICKS    0x10
#define HW_TIMER_GET_MS       0x11
#define HW_TIMER_GET_FREQ     0x12

#define HW_RTC_GET_TIME       0x13
#define HW_RTC_GET_UNIX64     0x14
#define HW_RTC_GET_UPTIME     0x15

#define HW_DISK_FLUSH         0x50
#define HW_DISK_INFO          0x51

/* The screen is lent to the one system program that draws (screen.h). */
#define HW_SCREEN_CLAIM         0x60
#define HW_SCREEN_RELEASE       0x61

#define HW_SYSTEM_REBOOT        0x80
#define HW_SYSTEM_SHUTDOWN      0x81
#define HW_DEBUG_PRINT          0x82
#define HW_LOG_READ             0x83
/* The account of the run BEFORE this one, carried through a warm reset in a
 * fixed physical window (klib_logring.h). Same wire shape as HW_LOG_READ so
 * one reader in userspace serves both. */
#define HW_LOG_PREVIOUS         0x84
/* A program's run of text into the log ring, and the bell the screen's holder
 * hangs on it (klib_logring.h). */
#define HW_LOG_SAY              0x85
#define HW_LOG_BELL             0x86

/* HW_LOG_READ's optional flags word. */
#define HW_LOG_READ_TONES       0x01u

/* An HW_LOG_READ answer: a header of HW_LOG_READ_HEADER bytes, then at most
 * HW_LOG_READ_CHUNK bytes of the ring — with HW_LOG_READ_TONES, as many tones
 * after them. */
#define HW_LOG_READ_HEADER      24u
#define HW_LOG_READ_CHUNK       4096u

/* The most one HW_LOG_SAY takes: a page. A longer run is several. */
#define HW_LOG_SAY_MAX          4096u

#define HW_USB_RESET            0x90
#define HW_USB_START            0x91
#define HW_USB_PORT_STATUS      0x92
#define HW_USB_PORT_RESET       0x93
#define HW_USB_ENUM_DEVICE      0x94
#define HW_USB_GET_INFO         0x95
#define HW_USB_PORT_QUERY       0x96

/* ============================================================================
 *  Operations deck opcodes — the same single source, for the kernel's handler
 *  table (decks/operations/operations_ops.c, which says each op's crates and
 *  params) and every program that builds a Manifest of them.
 * ============================================================================ */
#define OP_BUF_MOVE     0x01
#define OP_BUF_FILL     0x02
#define OP_BUF_XOR      0x03
#define OP_BUF_HASH     0x04
#define OP_BUF_CMP      0x05
#define OP_BUF_FIND     0x06
#define OP_BUF_PACK     0x07
#define OP_BUF_UNPACK   0x08
#define OP_BIT_SWAP     0x09
#define OP_VAL_ADD      0x0A

#endif // BOXOS_DECKS_H
