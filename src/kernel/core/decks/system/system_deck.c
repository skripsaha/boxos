/* ============================================================
 * System Deck — BoxOS
 *
 * Opcodes:
 *   0x01 PROC_SPAWN    0x02 PROC_KILL      0x03 PROC_INFO
 *   0x04 CTX_USE       0x06 PROC_EXEC
 *   0x10 BUF_ALLOC     0x11 BUF_FREE       0x12 BUF_RESIZE
 *   0x18 DEFRAG_FILE   0x19 FRAG_SCORE
 *   0x20 TAG_ADD       0x21 TAG_REMOVE     0x22 TAG_CHECK
 *   0x40 ROUTE         0x41 ROUTE_TAG      0x42 LISTEN
 * ============================================================ */

// --- Includes ---
#include "system_deck.h"
#include "klib.h"
#include "process.h"
#include "scheduler.h"
#include "tagfs.h"
#include "atomics.h"
#include "pocket.h"
#include "vmm.h"
#include "pmm.h"
#include "result_ring.h"
#include "error.h"
#include "listen_table.h"
#include "pocket_ring.h"
#include "ready_queue.h"
#include "guide.h"
#include "kernel_config.h"

// --- Constants ---
#define PROC_SPAWN_MAX_BINARY_SIZE   CONFIG_PROC_MAX_BINARY_SIZE
#define PROC_SPAWN_MAX_PHYS_ADDR     0x100000000ULL
#define PROC_INFO_TAGS_SIZE          64

#define BUF_MAX_SIZE                 CONFIG_PROC_MAX_BUFFER_SIZE
#define BUF_MAX_COUNT                64

#define MAX_CTX_USE_TAGS             3
#define CTX_TAG_LENGTH               64
#define CTX_USE_PARSE_BUF_SIZE       512

#define MAX_ROUTE_TAG_TARGETS        16

// --- Internal helpers ---

static void* get_request_data(Pocket* pocket, process_t* proc) {
    if (!pocket || pocket->data_length == 0) {
        return NULL;
    }
    if (!proc) {
        return NULL;
    }
    return vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
}


// --- Security Gate ---

static bool system_deck_check_permission(process_t* proc, uint8_t opcode) {
    if (!proc) return false;
    uint64_t t = proc->tag_bits;

    switch (opcode) {
        case SYSTEM_OP_PROC_SPAWN:
        case SYSTEM_OP_PROC_EXEC:
            return t & (g_well_known.utility | g_well_known.system);

        case SYSTEM_OP_PROC_KILL:
        case SYSTEM_OP_PROC_INFO:
            return true;

        case SYSTEM_OP_BUF_ALLOC:
        case SYSTEM_OP_BUF_FREE:
        case SYSTEM_OP_BUF_RESIZE:
            return t & (g_well_known.app | g_well_known.utility | g_well_known.system);

        case SYSTEM_OP_TAG_ADD:
        case SYSTEM_OP_TAG_REMOVE:
            return t & (g_well_known.utility | g_well_known.system);

        case SYSTEM_OP_TAG_CHECK:
        case SYSTEM_OP_CTX_USE:
            return true;

        case SYSTEM_OP_DEFRAG_FILE:
        case SYSTEM_OP_FRAG_SCORE:
            return t & (g_well_known.utility | g_well_known.system);

        case SYSTEM_OP_ROUTE:
        case SYSTEM_OP_ROUTE_TAG:
            return t & (g_well_known.app | g_well_known.utility | g_well_known.system);

        case SYSTEM_OP_LISTEN:
            return t & (g_well_known.app | g_well_known.utility | g_well_known.system);

        default:
            return false;
    }
}

bool system_security_gate(process_t* proc, uint8_t deck_id, uint8_t opcode) {
    if (!proc) return false;

    uint64_t t = proc->tag_bits;

    if (t & g_well_known.god)     return true;
    if (t & g_well_known.stopped) return false;

    switch (deck_id) {
        case 0x01:  // Operations Deck — open to all
            return true;

        case 0x02: {  // Storage Deck
            bool is_write = (opcode == 0x02 || opcode == 0x03 || opcode == 0x06 ||
                             opcode == 0x07 || opcode == 0x08 || opcode == 0x09 ||
                             opcode == 0x10 || opcode == 0x11);
            bool is_read  = (opcode == 0x01 || opcode == 0x05 || opcode == 0x0A);

            if (is_write)
                return t & (g_well_known.utility | g_well_known.system);
            if (is_read)
                return t & (g_well_known.app | g_well_known.utility | g_well_known.system);
            return t & g_well_known.system;
        }

        case 0x03:  // Hardware Deck
            return t & (g_well_known.system | g_well_known.bypass);

        case 0x04:  // Network Deck
            return t & (g_well_known.network | g_well_known.system);

        case DECK_SYSTEM:
            return system_deck_check_permission(proc, opcode);

        default:
            return false;
    }
}

// --- Buffer Management ---

typedef struct {
    uint64_t handle;
    uint64_t phys_addr;
    uint64_t virt_addr;
    uint64_t size;
    uint32_t owner_pid;
    bool     in_use;
} BufferEntry;

static BufferEntry buffer_table[BUF_MAX_COUNT];
static uint64_t    next_buffer_handle = 1;
static spinlock_t  buffer_table_lock  = {0};

static int find_free_buffer_slot(void) {
    for (int i = 0; i < BUF_MAX_COUNT; i++) {
        if (!buffer_table[i].in_use) {
            return i;
        }
    }
    return -1;
}

static int find_buffer_by_handle(uint64_t handle) {
    for (int i = 0; i < BUF_MAX_COUNT; i++) {
        if (buffer_table[i].in_use && buffer_table[i].handle == handle) {
            return i;
        }
    }
    return -1;
}

static uint32_t count_process_buffers(uint32_t pid) {
    uint32_t count = 0;
    for (int i = 0; i < BUF_MAX_COUNT; i++) {
        if (buffer_table[i].in_use && buffer_table[i].owner_pid == pid) {
            count++;
        }
    }
    return count;
}

static uint64_t generate_random_handle(void) {
    uint64_t tsc     = rdtsc();
    uint64_t counter = atomic_fetch_add_u64(&next_buffer_handle, 1);
    uint64_t result  = (tsc ^ (counter << 32) ^ (counter >> 32));
    return result ? result : 1;
}

void system_deck_cleanup_process_buffers(uint32_t pid) {
    process_t* proc = process_find(pid);

    spin_lock(&buffer_table_lock);

    for (int i = 0; i < BUF_MAX_COUNT; i++) {
        if (!buffer_table[i].in_use || buffer_table[i].owner_pid != pid) {
            continue;
        }

        void*    phys_addr = (void*)buffer_table[i].phys_addr;
        uint64_t virt_addr = buffer_table[i].virt_addr;
        size_t   pages     = buffer_table[i].size / PMM_PAGE_SIZE;

        debug_printf("[SYSTEM_DECK] Cleaning up buffer handle=%lu for PID %u\n",
                     buffer_table[i].handle, pid);

        if (virt_addr != 0 && proc && proc->cabin) {
            vmm_unmap_pages(proc->cabin, virt_addr, pages);
        }

        pmm_free(phys_addr, pages);
        __sync_synchronize();

        buffer_table[i].in_use    = false;
        buffer_table[i].handle    = 0;
        buffer_table[i].phys_addr = 0;
        buffer_table[i].virt_addr = 0;
        buffer_table[i].size      = 0;
        buffer_table[i].owner_pid = 0;
    }

    spin_unlock(&buffer_table_lock);
}

static int buf_alloc(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    uint8_t* data = (uint8_t*)get_request_data(pocket, proc);
    if (!data) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    // Request layout: [uint64_t size, uint32_t flags]
    uint64_t size  = *(uint64_t*)(data + 0);
    // flags not used currently but preserved for binary compat

    debug_printf("[SYSTEM_DECK] BUF_ALLOC: size=%lu\n", size);

    if (size == 0) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }
    if (size > BUF_MAX_SIZE) {
        pocket->error_code = ERR_BINARY_TOO_LARGE;
        return -1;
    }
    if (size > UINT64_MAX - PMM_PAGE_SIZE) {
        pocket->error_code = ERR_BINARY_TOO_LARGE;
        return -1;
    }

    spin_lock(&buffer_table_lock);

    if (count_process_buffers(pocket->pid) >= BUF_MAX_COUNT / 4) {
        spin_unlock(&buffer_table_lock);
        pocket->error_code = ERR_BUFFER_LIMIT_EXCEEDED;
        return -1;
    }

    int slot = find_free_buffer_slot();
    if (slot < 0) {
        spin_unlock(&buffer_table_lock);
        pocket->error_code = ERR_BUFFER_LIMIT_EXCEEDED;
        return -1;
    }

    uint64_t handle = generate_random_handle();
    int collision   = 0;
    while (find_buffer_by_handle(handle) >= 0) {
        handle = generate_random_handle();
        if (++collision > BUF_MAX_COUNT) {
            spin_unlock(&buffer_table_lock);
            pocket->error_code = ERR_BUFFER_LIMIT_EXCEEDED;
            return -1;
        }
    }

    spin_unlock(&buffer_table_lock);

    size_t pages   = (size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void*  phys    = pmm_alloc_zero(pages);
    __sync_synchronize();

    if (!phys) {
        pocket->error_code = ERR_NO_MEMORY;
        return -1;
    }

    spin_lock(&buffer_table_lock);
    if (buffer_table[slot].in_use) {
        spin_unlock(&buffer_table_lock);
        pmm_free(phys, pages);
        pocket->error_code = ERR_BUFFER_LIMIT_EXCEEDED;
        return -1;
    }

    buffer_table[slot].handle    = handle;
    buffer_table[slot].phys_addr = (uint64_t)phys;
    buffer_table[slot].size      = pages * PMM_PAGE_SIZE;
    buffer_table[slot].owner_pid = pocket->pid;
    buffer_table[slot].virt_addr = 0;
    buffer_table[slot].in_use    = true;
    spin_unlock(&buffer_table_lock);

    uint64_t virt = 0;
    if (proc && proc->cabin) {
        virt = proc->buf_heap_next;
        vmm_map_result_t mr = vmm_map_pages(proc->cabin, virt, (uintptr_t)phys,
                                             pages, VMM_FLAGS_USER_RW);
        if (mr.success) {
            proc->buf_heap_next += pages * PMM_PAGE_SIZE;
            spin_lock(&buffer_table_lock);
            buffer_table[slot].virt_addr = virt;
            spin_unlock(&buffer_table_lock);
        } else {
            virt = 0;
        }
    }

    // Response layout: [uint64_t buffer_handle, uint64_t phys_addr,
    //                   uint64_t actual_size, uint64_t virt_addr, uint32_t error_code]
    uint64_t actual_size = pages * PMM_PAGE_SIZE;
    *(uint64_t*)(data +  0) = handle;
    *(uint64_t*)(data +  8) = (uint64_t)phys;
    *(uint64_t*)(data + 16) = actual_size;
    *(uint64_t*)(data + 24) = virt;
    *(uint32_t*)(data + 32) = OK;

    pocket->error_code = OK;
    debug_printf("[SYSTEM_DECK] BUF_ALLOC: SUCCESS handle=%lu phys=0x%lx virt=0x%lx size=%lu\n",
                 handle, (uint64_t)phys, virt, actual_size);
    return 0;
}

static int buf_free(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    uint8_t* data = (uint8_t*)get_request_data(pocket, proc);
    if (!data) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    // Request layout: [uint64_t buffer_handle]
    uint64_t handle = *(uint64_t*)(data + 0);

    debug_printf("[SYSTEM_DECK] BUF_FREE: handle=%lu\n", handle);

    if (handle == 0) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    spin_lock(&buffer_table_lock);

    int slot = find_buffer_by_handle(handle);
    if (slot < 0) {
        spin_unlock(&buffer_table_lock);
        pocket->error_code = ERR_INVALID_BUFFER_ID;
        return -1;
    }

    if (buffer_table[slot].owner_pid != pocket->pid) {
        spin_unlock(&buffer_table_lock);
        pocket->error_code = ERR_ACCESS_DENIED;
        return -1;
    }

    void*    phys      = (void*)buffer_table[slot].phys_addr;
    uint64_t virt      = buffer_table[slot].virt_addr;
    size_t   pages     = buffer_table[slot].size / PMM_PAGE_SIZE;
    uint32_t owner_pid = buffer_table[slot].owner_pid;

    buffer_table[slot].in_use    = false;
    buffer_table[slot].handle    = 0;
    buffer_table[slot].phys_addr = 0;
    buffer_table[slot].virt_addr = 0;
    buffer_table[slot].size      = 0;
    buffer_table[slot].owner_pid = 0;
    spin_unlock(&buffer_table_lock);

    if (virt != 0) {
        process_t* proc = process_find(owner_pid);
        if (proc && proc->cabin) {
            vmm_unmap_pages(proc->cabin, virt, pages);
        }
    }

    pmm_free(phys, pages);
    __sync_synchronize();

    // Response layout: [uint32_t error_code]
    *(uint32_t*)(data + 0) = OK;

    pocket->error_code = OK;
    debug_printf("[SYSTEM_DECK] BUF_FREE: SUCCESS handle=%lu\n", handle);
    return 0;
}

static int buf_resize(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    uint8_t* data = (uint8_t*)get_request_data(pocket, proc);
    if (!data) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    // Request layout: [uint64_t buffer_handle, uint64_t new_size]
    uint64_t handle   = *(uint64_t*)(data + 0);
    uint64_t new_size = *(uint64_t*)(data + 8);

    debug_printf("[SYSTEM_DECK] BUF_RESIZE: handle=%lu new_size=%lu\n", handle, new_size);

    if (handle == 0) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }
    if (new_size == 0) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }
    if (new_size > BUF_MAX_SIZE) {
        pocket->error_code = ERR_BINARY_TOO_LARGE;
        return -1;
    }

    spin_lock(&buffer_table_lock);

    int slot = find_buffer_by_handle(handle);
    if (slot < 0) {
        spin_unlock(&buffer_table_lock);
        pocket->error_code = ERR_INVALID_BUFFER_ID;
        return -1;
    }

    if (buffer_table[slot].owner_pid != pocket->pid) {
        spin_unlock(&buffer_table_lock);
        pocket->error_code = ERR_ACCESS_DENIED;
        return -1;
    }

    uint64_t old_phys = buffer_table[slot].phys_addr;
    uint64_t old_virt = buffer_table[slot].virt_addr;
    uint64_t old_size = buffer_table[slot].size;
    size_t   old_pages = old_size / PMM_PAGE_SIZE;
    size_t   new_pages = (new_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t new_actual = new_pages * PMM_PAGE_SIZE;

    if (new_pages == old_pages) {
        spin_unlock(&buffer_table_lock);
        // Response layout: [uint64_t buffer_handle, uint64_t actual_size, uint32_t error_code]
        *(uint64_t*)(data +  0) = handle;
        *(uint64_t*)(data +  8) = new_actual;
        *(uint32_t*)(data + 16) = OK;
        pocket->error_code = OK;
        return 0;
    }

    spin_unlock(&buffer_table_lock);

    void* new_phys = pmm_alloc_zero(new_pages);
    if (!new_phys) {
        pocket->error_code = ERR_NO_MEMORY;
        return -1;
    }

    void*  old_virt_kern = vmm_phys_to_virt(old_phys);
    void*  new_virt_kern = vmm_phys_to_virt((uintptr_t)new_phys);
    size_t copy_size     = old_size < new_actual ? old_size : new_actual;
    memcpy(new_virt_kern, old_virt_kern, copy_size);
    __sync_synchronize();

    uint64_t   new_virt = old_virt;

    if (old_virt != 0 && proc && proc->cabin) {
        vmm_unmap_pages(proc->cabin, old_virt, old_pages);
        if (new_pages <= old_pages) {
            vmm_map_pages(proc->cabin, old_virt, (uintptr_t)new_phys,
                          new_pages, VMM_FLAGS_USER_RW);
        } else {
            new_virt = proc->buf_heap_next;
            vmm_map_result_t mr = vmm_map_pages(proc->cabin, new_virt,
                                                 (uintptr_t)new_phys, new_pages,
                                                 VMM_FLAGS_USER_RW);
            if (mr.success) {
                proc->buf_heap_next += new_pages * PMM_PAGE_SIZE;
            } else {
                new_virt = 0;
            }
        }
    }

    spin_lock(&buffer_table_lock);
    if (!buffer_table[slot].in_use || buffer_table[slot].handle != handle) {
        spin_unlock(&buffer_table_lock);
        pmm_free(new_phys, new_pages);
        pocket->error_code = ERR_INVALID_BUFFER_ID;
        return -1;
    }
    buffer_table[slot].phys_addr = (uint64_t)new_phys;
    buffer_table[slot].virt_addr = new_virt;
    buffer_table[slot].size      = new_actual;
    spin_unlock(&buffer_table_lock);

    pmm_free((void*)old_phys, old_pages);

    // Response layout: [uint64_t buffer_handle, uint64_t actual_size, uint32_t error_code]
    *(uint64_t*)(data +  0) = handle;
    *(uint64_t*)(data +  8) = new_actual;
    *(uint32_t*)(data + 16) = OK;

    pocket->error_code = OK;
    debug_printf("[SYSTEM_DECK] BUF_RESIZE: SUCCESS handle=%lu old=%lu new=%lu\n",
                 handle, old_size, new_actual);
    return 0;
}

// --- Process Operations ---

static int proc_spawn(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    uint8_t* data = (uint8_t*)get_request_data(pocket, proc);
    if (!data) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: failed to read request data\n");
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    // Request layout: [uint64_t binary_phys_addr, uint64_t binary_size, char tags[128]]
    uint64_t binary_phys = *(uint64_t*)(data + 0);
    uint64_t binary_size = *(uint64_t*)(data + 8);
    char*    tags        = (char*)(data + 16);

    if (strnlen(tags, 128) == 128) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: tags not null-terminated\n");
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }
    tags[127] = '\0';

    if (tags[0] == '\0') {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: empty tags\n");
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    debug_printf("[SYSTEM_DECK] PROC_SPAWN: binary_phys=0x%lx size=%lu tags='%s'\n",
                 binary_phys, binary_size, tags);

    if (binary_phys == 0 || binary_size == 0) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }
    if (binary_phys & 0xFFF) {
        pocket->error_code = ERR_ALIGNMENT;
        return -1;
    }
    if (binary_phys + binary_size < binary_phys) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }
    if (binary_phys > PROC_SPAWN_MAX_PHYS_ADDR) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }
    if (binary_size > (SIZE_MAX - 4096)) {
        pocket->error_code = ERR_BINARY_TOO_LARGE;
        return -1;
    }
    if (binary_size > PROC_SPAWN_MAX_BINARY_SIZE) {
        pocket->error_code = ERR_BINARY_TOO_LARGE;
        return -1;
    }
    if (process_get_count() >= PROCESS_MAX_COUNT) {
        pocket->error_code = ERR_PROCESS_LIMIT_EXCEEDED;
        return -1;
    }

    process_t* new_proc = process_create(tags);
    if (!new_proc) {
        pocket->error_code = ERR_SPAWN_FAILED;
        return -1;
    }
    new_proc->spawner_pid = pocket->pid;

    const uint8_t* elf = (const uint8_t*)vmm_phys_to_virt(binary_phys);
    if (binary_size < 16 || elf[0] != 0x7F || elf[1] != 'E' ||
        elf[2] != 'L' || elf[3] != 'F') {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: invalid ELF magic\n");
        process_destroy(new_proc);
        pocket->error_code = ERR_INVALID_ELF;
        return -1;
    }

    if (process_load_binary(new_proc, (void*)elf, binary_size) != 0) {
        debug_printf("[SYSTEM_DECK] PROC_SPAWN: load binary failed\n");
        process_destroy(new_proc);
        pocket->error_code = ERR_SPAWN_FAILED;
        return -1;
    }

    // Response layout: [uint32_t new_pid, uint32_t reserved]
    *(uint32_t*)(data + 0) = new_proc->pid;
    *(uint32_t*)(data + 4) = 0;

    pocket->error_code = OK;
    debug_printf("[SYSTEM_DECK] PROC_SPAWN: SUCCESS PID %u\n", new_proc->pid);
    return 0;
}

static int proc_kill(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    uint32_t target_pid;
    bool     is_self_exit;
    uint8_t* data = NULL;

    if (pocket->data_length == 0) {
        target_pid = pocket->pid;
        is_self_exit = true;
#if CONFIG_DEBUG_PROCESS
        debug_printf("[SYSTEM_DECK] PROC_KILL: PID %u graceful exit (no data)\n", target_pid);
#endif
    } else {
        data = (uint8_t*)get_request_data(pocket, proc);
        if (!data || pocket->data_length < 4) {
            pocket->error_code = ERR_INVALID_ARGUMENT;
            return -1;
        }

        // Request layout: [uint32_t target_pid, uint32_t reserved]
        target_pid = *(uint32_t*)(data + 0);
        is_self_exit = (target_pid == 0);

        if (is_self_exit) {
            target_pid = pocket->pid;
#if CONFIG_DEBUG_PROCESS
            debug_printf("[SYSTEM_DECK] PROC_KILL: PID %u graceful exit\n", target_pid);
        } else {
            debug_printf("[SYSTEM_DECK] PROC_KILL: killing PID %u\n", target_pid);
#else
        } else {
            (void)0;
#endif
        }
    }

    if (target_pid == PROCESS_INVALID_PID) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    process_t* target = process_find(target_pid);
    if (!target) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    if (is_self_exit) {
        process_set_state(target, PROC_DONE);
    } else {
        process_set_state(target, PROC_CRASHED);
    }
    __sync_synchronize();

    system_deck_cleanup_process_buffers(target_pid);

    if (data && pocket->data_length >= 8) {
        // Response layout: [uint32_t killed_pid, uint32_t reserved]
        *(uint32_t*)(data + 0) = target_pid;
        *(uint32_t*)(data + 4) = 0;
    }

    pocket->error_code = OK;
#if CONFIG_DEBUG_PROCESS
    debug_printf("[SYSTEM_DECK] PROC_KILL: SUCCESS\n");
#endif
    return 0;
}

static int proc_info(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    uint8_t* data = (uint8_t*)get_request_data(pocket, proc);
    if (!data) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    // Request layout: [uint32_t target_pid, uint32_t reserved]
    uint32_t target_pid = *(uint32_t*)(data + 0);

    if (target_pid == 0) {
        target_pid = pocket->pid;
        debug_printf("[SYSTEM_DECK] PROC_INFO: self (PID %u)\n", target_pid);
    } else {
        debug_printf("[SYSTEM_DECK] PROC_INFO: PID %u\n", target_pid);
    }

    process_t* target = process_find(target_pid);
    if (!target) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    // Response layout: [uint32_t pid, uint32_t state, int32_t score, uint32_t reserved1,
    //                   uint64_t code_start, uint64_t code_size, char tags[64]]
    *(uint32_t*)(data +  0) = target->pid;
    *(uint32_t*)(data +  4) = (uint32_t)target->state;
    *(int32_t* )(data +  8) = target->score;
    *(uint32_t*)(data + 12) = 0;
    *(uint64_t*)(data + 16) = target->code_start;
    *(uint64_t*)(data + 24) = target->code_size;
    process_snapshot_tags(target, (char*)(data + 32), PROC_INFO_TAGS_SIZE);

    pocket->error_code = OK;
    debug_printf("[SYSTEM_DECK] PROC_INFO: SUCCESS PID %u\n", target->pid);
    return 0;
}

static int proc_exec(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    uint8_t* data = (uint8_t*)get_request_data(pocket, proc);
    if (!data) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    // Request layout: [char filename[32], uint8_t reserved[160]]
    char* filename = (char*)(data + 0);

    if (strnlen(filename, 32) == 32) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: filename not null-terminated\n");
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }
    filename[31] = '\0';

    if (filename[0] == '\0') {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: empty filename\n");
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

#if CONFIG_DEBUG_PROCESS
    debug_printf("[SYSTEM_DECK] PROC_EXEC: looking for '%s'\n", filename);
#endif

    if (process_get_count() >= PROCESS_MAX_COUNT) {
        pocket->error_code = ERR_PROCESS_LIMIT_EXCEEDED;
        return -1;
    }

    #define PROC_EXEC_MAX_SCAN 256
    uint32_t* file_ids = kmalloc(PROC_EXEC_MAX_SCAN * sizeof(uint32_t));
    if (!file_ids) {
        pocket->error_code = ERR_NO_MEMORY;
        return -1;
    }

    int file_count = tagfs_list_all_files(file_ids, PROC_EXEC_MAX_SCAN);

    uint32_t found_id = 0;
    char     found_tags[PROCESS_TAG_SIZE];
    found_tags[0] = '\0';

    TagFSState* tfs = tagfs_get_state();

    for (int i = 0; i < file_count; i++) {
        TagFSMetadata meta;
        if (tagfs_get_metadata(file_ids[i], &meta) != 0) {
            continue;
        }
        if (!(meta.flags & TAGFS_FILE_ACTIVE)) {
            tagfs_metadata_free(&meta);
            continue;
        }

        bool has_name     = false;
        bool has_exec_tag = false;

        for (uint16_t t = 0; t < meta.tag_count; t++) {
            const char* key = tfs ? tag_registry_key(tfs->registry, meta.tag_ids[t]) : NULL;
            if (!key) continue;
            if (strcmp(key, filename) == 0) {
                has_name = true;
            }
            if (strcmp(key, "app") == 0 || strcmp(key, "utility") == 0) {
                has_exec_tag = true;
            }
        }

        if (has_name && has_exec_tag) {
            found_id = file_ids[i];

            size_t pos = 0;
            for (uint16_t t = 0; t < meta.tag_count; t++) {
                const char* key = tfs ? tag_registry_key(tfs->registry, meta.tag_ids[t]) : NULL;
                if (!key) continue;
                size_t klen = strlen(key);
                if (pos + klen + 2 > PROCESS_TAG_SIZE) {
                    break;
                }
                if (pos > 0) {
                    found_tags[pos++] = ',';
                }
                memcpy(found_tags + pos, key, klen);
                pos += klen;
            }
            found_tags[pos] = '\0';
            tagfs_metadata_free(&meta);
            break;
        }
        tagfs_metadata_free(&meta);
    }

    kfree(file_ids);

    if (found_id == 0) {
        debug_printf("[SYSTEM_DECK] PROC_EXEC: '%s' not found\n", filename);
        pocket->error_code = ERR_FILE_NOT_FOUND;
        return -1;
    }

    TagFSMetadata exec_meta;
    if (tagfs_get_metadata(found_id, &exec_meta) != 0) {
        pocket->error_code = ERR_FILE_NOT_FOUND;
        return -1;
    }
    uint64_t file_size = exec_meta.size;
    tagfs_metadata_free(&exec_meta);

    if (file_size == 0 || file_size > PROC_SPAWN_MAX_BINARY_SIZE) {
        pocket->error_code = ERR_BINARY_TOO_LARGE;
        return -1;
    }

    size_t pages_needed = (file_size + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    void*  phys_buf     = pmm_alloc_zero(pages_needed);
    if (!phys_buf) {
        pocket->error_code = ERR_NO_MEMORY;
        return -1;
    }

    void* virt_buf = vmm_phys_to_virt((uintptr_t)phys_buf);

    TagFSFileHandle* fh = tagfs_open(found_id, TAGFS_HANDLE_READ);
    if (!fh) {
        pmm_free(phys_buf, pages_needed);
        pocket->error_code = ERR_SPAWN_FAILED;
        return -1;
    }

    int read_result = tagfs_read(fh, virt_buf, file_size);
    tagfs_close(fh);

    if (read_result < 0) {
        pmm_free(phys_buf, pages_needed);
        pocket->error_code = ERR_SPAWN_FAILED;
        return -1;
    }

    process_t* new_proc = process_create(found_tags);
    if (!new_proc) {
        pmm_free(phys_buf, pages_needed);
        pocket->error_code = ERR_SPAWN_FAILED;
        return -1;
    }

    new_proc->spawner_pid = pocket->pid;

    int load_result = process_load_binary(new_proc, virt_buf, (size_t)file_size);
    pmm_free(phys_buf, pages_needed);

    if (load_result != 0) {
        process_destroy(new_proc);
        pocket->error_code = ERR_SPAWN_FAILED;
        return -1;
    }

    __sync_synchronize();
    process_set_state(new_proc, PROC_WORKING);

    // Response layout: [uint32_t new_pid, uint32_t reserved, uint8_t reserved2[184]]
    *(uint32_t*)(data + 0) = new_proc->pid;
    *(uint32_t*)(data + 4) = 0;

    pocket->error_code = OK;
#if CONFIG_DEBUG_PROCESS
    debug_printf("[SYSTEM_DECK] PROC_EXEC: SUCCESS '%s' -> PID %u\n", filename, new_proc->pid);
#endif
    return 0;
}

// --- Tag Operations ---

static int tag_add(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    uint8_t* data = (uint8_t*)get_request_data(pocket, proc);
    if (!data) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    // Request layout: [uint32_t target_pid, char tag[64], uint8_t reserved[124]]
    uint32_t target_pid = *(uint32_t*)(data + 0);
    char*    tag        = (char*)(data + 4);

    if (strnlen(tag, 64) == 64) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }
    tag[63] = '\0';

    if (tag[0] == '\0') {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    debug_printf("[SYSTEM_DECK] TAG_ADD: target_pid=%u tag='%s'\n", target_pid, tag);

    if (target_pid == PROCESS_INVALID_PID) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    process_t* target = process_find(target_pid);
    if (!target) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    if (process_has_tag(target, tag)) {
        // Response layout: [uint32_t error_code, char message[128]]
        *(uint32_t*)(data + 0) = ERR_ALREADY_EXISTS;
        strncpy((char*)(data + 4), "Tag already exists", 127);
        ((char*)(data + 4))[127] = '\0';
        pocket->error_code = ERR_ALREADY_EXISTS;
        return -1;
    }

    if (process_add_tag(target, tag) != 0) {
        *(uint32_t*)(data + 0) = ERR_TAG_LIMIT_EXCEEDED;
        strncpy((char*)(data + 4), "Tag buffer full", 127);
        ((char*)(data + 4))[127] = '\0';
        pocket->error_code = ERR_TAG_LIMIT_EXCEEDED;
        return -1;
    }

    if (strcmp(tag, "stopped") == 0) {
        process_state_t state = process_get_state(target);
        if (state == PROC_WORKING || state == PROC_CREATED) {
            process_set_state(target, PROC_STOPPED);
        }
    }

    *(uint32_t*)(data + 0) = OK;
    strncpy((char*)(data + 4), "Tag added successfully", 127);
    ((char*)(data + 4))[127] = '\0';

    pocket->error_code = OK;
    debug_printf("[SYSTEM_DECK] TAG_ADD: SUCCESS tag='%s' PID %u\n", tag, target_pid);
    return 0;
}

static int tag_remove(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    uint8_t* data = (uint8_t*)get_request_data(pocket, proc);
    if (!data) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    // Request layout: [uint32_t target_pid, char tag[64], uint8_t reserved[124]]
    uint32_t target_pid = *(uint32_t*)(data + 0);
    char*    tag        = (char*)(data + 4);

    if (strnlen(tag, 64) == 64) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }
    tag[63] = '\0';

    if (tag[0] == '\0') {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    debug_printf("[SYSTEM_DECK] TAG_REMOVE: target_pid=%u tag='%s'\n", target_pid, tag);

    if (target_pid == PROCESS_INVALID_PID) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    process_t* target = process_find(target_pid);
    if (!target) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    if (!process_has_tag(target, tag)) {
        *(uint32_t*)(data + 0) = ERR_TAG_NOT_FOUND;
        strncpy((char*)(data + 4), "Tag not found", 127);
        ((char*)(data + 4))[127] = '\0';
        pocket->error_code = ERR_TAG_NOT_FOUND;
        return -1;
    }

    if (process_remove_tag(target, tag) != 0) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    if (strcmp(tag, "stopped") == 0) {
        if (process_get_state(target) == PROC_STOPPED) {
            process_set_state(target, PROC_WORKING);
        }
    }

    *(uint32_t*)(data + 0) = OK;
    strncpy((char*)(data + 4), "Tag removed successfully", 127);
    ((char*)(data + 4))[127] = '\0';

    pocket->error_code = OK;
    debug_printf("[SYSTEM_DECK] TAG_REMOVE: SUCCESS tag='%s' PID %u\n", tag, target_pid);
    return 0;
}

static int tag_check(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    uint8_t* data = (uint8_t*)get_request_data(pocket, proc);
    if (!data) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    // Request layout: [uint32_t target_pid, char tag[64], uint8_t reserved[124]]
    uint32_t target_pid = *(uint32_t*)(data + 0);
    char*    tag        = (char*)(data + 4);

    if (strnlen(tag, 64) == 64) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }
    tag[63] = '\0';

    if (tag[0] == '\0') {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    debug_printf("[SYSTEM_DECK] TAG_CHECK: target_pid=%u tag='%s'\n", target_pid, tag);

    if (target_pid == PROCESS_INVALID_PID) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    process_t* target = process_find(target_pid);
    if (!target) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    bool has = process_has_tag(target, tag);

    // Response layout: [bool has_tag (1 byte), uint8_t reserved1[3], uint32_t error_code]
    *(uint8_t* )(data + 0) = has ? 1 : 0;
    *(uint8_t* )(data + 1) = 0;
    *(uint8_t* )(data + 2) = 0;
    *(uint8_t* )(data + 3) = 0;
    *(uint32_t*)(data + 4) = OK;

    pocket->error_code = OK;
    debug_printf("[SYSTEM_DECK] TAG_CHECK: PID %u %s tag '%s'\n",
                 target_pid, has ? "has" : "lacks", tag);
    return 0;
}

// --- Context Operations ---

static int ctx_use_parse_tags(const char* input, char tags[][CTX_TAG_LENGTH],
                               uint32_t* tag_count) {
    if (!input || !tags || !tag_count) {
        return -1;
    }

    *tag_count = 0;

    if (input[0] == '\0') {
        return 0;
    }

    char buf[CTX_USE_PARSE_BUF_SIZE];
    strncpy(buf, input, CTX_USE_PARSE_BUF_SIZE - 1);
    buf[CTX_USE_PARSE_BUF_SIZE - 1] = '\0';

    char* saveptr = NULL;
    char* token   = strtok_r(buf, ",", &saveptr);

    while (token != NULL) {
        while (*token == ' ' || *token == '\t') {
            token++;
        }
        char* end = token + strlen(token) - 1;
        while (end > token && (*end == ' ' || *end == '\t')) {
            *end-- = '\0';
        }

        if (*token == '\0') {
            token = strtok_r(NULL, ",", &saveptr);
            continue;
        }

        if (*tag_count >= MAX_CTX_USE_TAGS) {
            return ERR_TAG_LIMIT_EXCEEDED;
        }

        if (strlen(token) >= CTX_TAG_LENGTH) {
            return ERR_INVALID_ARGUMENT;
        }

        strncpy(tags[*tag_count], token, CTX_TAG_LENGTH - 1);
        tags[*tag_count][CTX_TAG_LENGTH - 1] = '\0';
        (*tag_count)++;

        token = strtok_r(NULL, ",", &saveptr);
    }

    return 0;
}

static int ctx_use(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    if (!proc) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    uint8_t* data = (uint8_t*)vmm_translate_user_addr(proc->cabin, pocket->data_addr,
                                                        pocket->data_length);
    if (!data) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    // Request layout: [char context_string[512]]
    // Response layout: [uint32_t error_code, char message[128], uint8_t reserved[60]]
    const char* context_string = (const char*)data;

    char parsed_tags[MAX_CTX_USE_TAGS][CTX_TAG_LENGTH];
    uint32_t tag_count = 0;
    memset(parsed_tags, 0, sizeof(parsed_tags));

    int parse_result = ctx_use_parse_tags(context_string, parsed_tags, &tag_count);

    if (parse_result != 0) {
        *(uint32_t*)(data + 0) = (uint32_t)parse_result;
        const char* msg;
        if (parse_result == ERR_TAG_LIMIT_EXCEEDED) {
            msg = "Too many tags (max 3)";
        } else {
            msg = "Invalid format or tag too long";
        }
        strncpy((char*)(data + 4), msg, 127);
        ((char*)(data + 4))[127] = '\0';
        pocket->error_code = (uint32_t)parse_result;
        debug_printf("[CTX_USE] Parse error code %d\n", parse_result);
        return -1;
    }

    if (tag_count == 0) {
        scheduler_clear_use_context();
        *(uint32_t*)(data + 0) = OK;
        strncpy((char*)(data + 4), "Context cleared", 127);
        ((char*)(data + 4))[127] = '\0';
        pocket->error_code = OK;
        debug_printf("[CTX_USE] Context cleared\n");
        return 0;
    }

    const char* tag_ptrs[MAX_CTX_USE_TAGS];
    for (uint32_t i = 0; i < tag_count; i++) {
        tag_ptrs[i] = parsed_tags[i];
    }
    scheduler_set_use_context(tag_ptrs, tag_count);

    char context_desc[128];
    size_t offset = 0;
    strncpy(context_desc, "Context set: ", sizeof(context_desc) - 1);
    offset = strlen(context_desc);
    for (uint32_t i = 0; i < tag_count && offset < sizeof(context_desc) - 1; i++) {
        if (i > 0 && offset < sizeof(context_desc) - 2) {
            context_desc[offset++] = ',';
            context_desc[offset++] = ' ';
        }
        size_t remaining = sizeof(context_desc) - offset - 1;
        strncpy(context_desc + offset, parsed_tags[i], remaining);
        offset += strlen(parsed_tags[i]);
        if (offset >= sizeof(context_desc) - 1) {
            offset = sizeof(context_desc) - 1;
            break;
        }
    }
    context_desc[offset] = '\0';

    *(uint32_t*)(data + 0) = OK;
    strncpy((char*)(data + 4), context_desc, 127);
    ((char*)(data + 4))[127] = '\0';

    pocket->error_code = OK;
    debug_printf("[CTX_USE] %s\n", context_desc);
    return 0;
}

// --- IPC Routing ---

static uint64_t ipc_copy_to_heap(process_t* sender, process_t* target,
                                  uint64_t src_addr, uint32_t length) {
    if (!sender || !target || length == 0 || src_addr == 0) {
        return 0;
    }

    void* src = vmm_translate_user_addr(sender->cabin, src_addr, length);
    if (!src) {
        return 0;
    }

    uint32_t pages_needed = (length + PMM_PAGE_SIZE - 1) / PMM_PAGE_SIZE;
    uint64_t target_vaddr = target->buf_heap_next;

    for (uint32_t i = 0; i < pages_needed; i++) {
        void* page = pmm_alloc(1);
        if (!page) {
            return 0;
        }
        uint64_t vaddr = target_vaddr + (i * PMM_PAGE_SIZE);
        vmm_map_result_t ret = vmm_map_page(target->cabin, vaddr, (uint64_t)page,
                                             VMM_FLAGS_USER_RW);
        if (!ret.success) {
            pmm_free(page, 1);
            return 0;
        }
    }

    target->buf_heap_next += pages_needed * PMM_PAGE_SIZE;

    void* dst = vmm_translate_user_addr(target->cabin, target_vaddr, length);
    if (!dst) {
        return 0;
    }

    memcpy(dst, src, length);
    return target_vaddr;
}

static bool tag_matches_process(process_t* proc, const char* route_tag) {
    if (!proc || !route_tag || route_tag[0] == '\0') {
        return false;
    }

    char tags[PROCESS_TAG_SIZE];
    process_snapshot_tags(proc, tags, sizeof(tags));

    const char* pos = tags;
    while (*pos) {
        const char* comma  = strchr(pos, ',');
        size_t      tag_len = comma ? (size_t)(comma - pos) : strlen(pos);

        char current[PROCESS_TAG_SIZE];
        size_t copy_len = tag_len < PROCESS_TAG_SIZE - 1 ? tag_len : PROCESS_TAG_SIZE - 1;
        memcpy(current, pos, copy_len);
        current[copy_len] = '\0';

        if (tag_match(route_tag, current)) {
            return true;
        }

        if (!comma) break;
        pos = comma + 1;
    }

    return false;
}

static int route(Pocket* pocket, process_t* proc) {
    if (!pocket) return -1;

    if (pocket->target_pid == 0) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    if (pocket->target_pid == pocket->pid) {
        pocket->error_code = ERR_ROUTE_SELF;
        return -1;
    }

    process_t* target = process_find(pocket->target_pid);
    if (!target) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    process_state_t target_state = process_get_state(target);
    if (target_state == PROC_CRASHED || target_state == PROC_DONE) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    if (pocket->data_length > 0 && pocket->data_addr != 0) {
        if (!proc) {
            pocket->error_code = ERR_PROCESS_NOT_FOUND;
            return -1;
        }

        uint64_t new_addr = ipc_copy_to_heap(proc, target,
                                              pocket->data_addr, pocket->data_length);
        if (new_addr == 0) {
            pocket->error_code = ERR_NO_MEMORY;
            return -1;
        }

        pocket->data_addr = new_addr;
    }

    pocket->error_code = OK;
#if CONFIG_DEBUG_WORKFLOW
    debug_printf("[ROUTE] PID %u -> PID %u\n", pocket->pid, pocket->target_pid);
#endif
    return 0;
}

static int route_tag(Pocket* pocket, process_t* proc) {
    if (!pocket) return -1;

    if (pocket->route_tag[0] == '\0') {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint32_t targets[MAX_ROUTE_TAG_TARGETS];
    uint32_t target_count = 0;

    process_t* iter = process_get_first();
    while (iter && target_count < MAX_ROUTE_TAG_TARGETS) {
        process_state_t pstate = process_get_state(iter);
        if ((pstate == PROC_WORKING || pstate == PROC_WAITING) &&
            iter->pid != pocket->pid) {
            if (tag_matches_process(iter, pocket->route_tag)) {
                targets[target_count++] = iter->pid;
            }
        }
        iter = iter->next;
    }

    if (target_count == 0) {
        pocket->error_code = ERR_ROUTE_NO_SUBSCRIBERS;
        return -1;
    }

    if (!proc) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    uint32_t delivered = 0;

    for (uint32_t i = 0; i < target_count; i++) {
        process_t* t = process_find(targets[i]);
        if (!t || !t->result_ring_phys) continue;

        ResultRing* rring = (ResultRing*)vmm_phys_to_virt(t->result_ring_phys);
        if (!rring) continue;

        uint64_t target_data_addr   = 0;
        uint32_t target_data_length = 0;

        if (pocket->data_length > 0 && pocket->data_addr != 0) {
            target_data_addr = ipc_copy_to_heap(proc, t,
                                                 pocket->data_addr, pocket->data_length);
            if (target_data_addr != 0) {
                target_data_length = pocket->data_length;
            }
        }

        Result result;
        result.error_code  = OK;
        result.data_length = target_data_length;
        result.data_addr   = target_data_addr;
        result.sender_pid  = pocket->pid;
        result._reserved   = 0;

        if (result_ring_push(rring, &result)) {
            delivered++;
            if (process_get_state(t) == PROC_WAITING) {
                process_set_state(t, PROC_WORKING);
            }
        }
    }

    if (delivered == 0) {
        pocket->error_code = ERR_ROUTE_TARGET_FULL;
        return -1;
    }

#if CONFIG_DEBUG_WORKFLOW
    debug_printf("[ROUTE_TAG] PID %u -> %u/%u targets matching '%s'\n",
                 pocket->pid, delivered, target_count, pocket->route_tag);
#endif

    pocket->target_pid = 0;
    pocket->error_code = OK;
    return 0;
}

static int listen(Pocket* pocket, process_t* proc) {
    if (!pocket) return -1;

    if (!proc) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    void* data = vmm_translate_user_addr(proc->cabin, pocket->data_addr, pocket->data_length);
    if (!data || pocket->data_length < 2) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint8_t* bytes      = (uint8_t*)data;
    uint8_t  source_type = bytes[0];
    uint8_t  flags       = bytes[1];

    int result = listen_table_add(pocket->pid, source_type, flags);

    if (result < 0) {
        error_t err;
        if (result == -ERR_LISTEN_ALREADY) {
            err = ERR_LISTEN_ALREADY;
        } else if (result == -ERR_LISTEN_TABLE_FULL) {
            err = ERR_LISTEN_TABLE_FULL;
        } else {
            err = ERR_INTERNAL;
        }
        pocket->error_code = (uint32_t)err;
        uint32_t err_out = (uint32_t)err;
        memcpy(data, &err_out, sizeof(uint32_t));
        return -1;
    }

    pocket->error_code = OK;
    uint32_t ok = 0;
    memcpy(data, &ok, sizeof(uint32_t));
    return 0;
}

// --- Filesystem Operations ---

static int defrag_file(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    if (!proc) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    uint8_t* data = (uint8_t*)vmm_translate_user_addr(proc->cabin, pocket->data_addr,
                                                        pocket->data_length);
    if (!data) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    // Request layout: [uint32_t file_id, uint32_t target_block]
    uint32_t file_id      = *(uint32_t*)(data + 0);
    uint32_t target_block = *(uint32_t*)(data + 4);

    int result = tagfs_defrag_file(file_id, target_block);

    // Response layout: [uint32_t error_code, uint32_t fragmentation_score]
    uint32_t score = tagfs_get_fragmentation_score();
    *(uint32_t*)(data + 0) = (result == 0) ? OK : ERR_INVALID_ARGUMENT;
    *(uint32_t*)(data + 4) = score;

    pocket->error_code = (result == 0) ? OK : ERR_INVALID_ARGUMENT;
    return (result == 0) ? 0 : -1;
}

static int frag_score(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        return -1;
    }

    if (!proc) {
        pocket->error_code = ERR_PROCESS_NOT_FOUND;
        return -1;
    }

    uint8_t* data = (uint8_t*)vmm_translate_user_addr(proc->cabin, pocket->data_addr,
                                                        pocket->data_length);
    if (!data) {
        pocket->error_code = ERR_INVALID_ARGUMENT;
        return -1;
    }

    uint32_t score       = tagfs_get_fragmentation_score();
    uint32_t total_files = 0;
    uint32_t total_gaps  = 0;

    TagFSState* fs_state = tagfs_get_state();
    if (fs_state && fs_state->initialized) {
        uint32_t max_id = fs_state->superblock.next_file_id;
        for (uint32_t i = 1; i < max_id; i++) {
            TagFSMetadata meta;
            if (tagfs_get_metadata(i, &meta) == 0) {
                if (meta.flags & TAGFS_FILE_ACTIVE) {
                    total_files++;
                }
                tagfs_metadata_free(&meta);
            }
        }
    }

    // Response layout: [uint32_t score, uint32_t total_files, uint32_t total_gaps]
    memset(data, 0, pocket->data_length);
    *(uint32_t*)(data + 0) = score;
    *(uint32_t*)(data + 4) = total_files;
    *(uint32_t*)(data + 8) = total_gaps;

    pocket->error_code = OK;
    return 0;
}

// --- Main Handler ---

int system_deck_handler(Pocket* pocket, process_t* proc) {
    if (!pocket) {
        debug_printf("[SYSTEM_DECK] ERROR: NULL pocket\n");
        return -1;
    }

    uint8_t opcode = pocket_get_opcode(pocket, pocket->current_prefix_idx);

    switch (opcode) {
        case SYSTEM_OP_PROC_SPAWN:   return proc_spawn(pocket, proc);
        case SYSTEM_OP_PROC_KILL:    return proc_kill(pocket, proc);
        case SYSTEM_OP_PROC_INFO:    return proc_info(pocket, proc);
        case SYSTEM_OP_PROC_EXEC:    return proc_exec(pocket, proc);
        case SYSTEM_OP_CTX_USE:      return ctx_use(pocket, proc);
        case SYSTEM_OP_BUF_ALLOC:    return buf_alloc(pocket, proc);
        case SYSTEM_OP_BUF_FREE:     return buf_free(pocket, proc);
        case SYSTEM_OP_BUF_RESIZE:   return buf_resize(pocket, proc);
        case SYSTEM_OP_TAG_ADD:      return tag_add(pocket, proc);
        case SYSTEM_OP_TAG_REMOVE:   return tag_remove(pocket, proc);
        case SYSTEM_OP_TAG_CHECK:    return tag_check(pocket, proc);
        case SYSTEM_OP_DEFRAG_FILE:  return defrag_file(pocket, proc);
        case SYSTEM_OP_FRAG_SCORE:   return frag_score(pocket, proc);
        case SYSTEM_OP_ROUTE:        return route(pocket, proc);
        case SYSTEM_OP_ROUTE_TAG:    return route_tag(pocket, proc);
        case SYSTEM_OP_LISTEN:       return listen(pocket, proc);

        default:
            debug_printf("[SYSTEM_DECK] Unknown opcode 0x%02x\n", opcode);
            pocket->error_code = ERR_NOT_IMPLEMENTED;
            return -1;
    }
}
