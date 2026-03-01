#include "cpu.h"
#include "klib.h"

static cpu_info_t cpu_info = {0};

void detect_cpu_info(char* cpu_vendor, char* cpu_brand) {
    uint32_t eax, ebx, ecx, edx;
    asm volatile("cpuid"
                 : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                 : "a"(0));
    *((uint32_t*)cpu_vendor) = ebx;
    *((uint32_t*)(cpu_vendor + 4)) = edx;
    *((uint32_t*)(cpu_vendor + 8)) = ecx;
    cpu_vendor[12] = '\0';
    asm volatile("cpuid"
                 : "=a"(eax)
                 : "a"(0x80000000));
    if (eax >= 0x80000004) {
        for (int i = 0; i < 3; i++) {
            asm volatile("cpuid"
                         : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                         : "a"(0x80000002 + i));
            *((uint32_t*)(cpu_brand + i * 16)) = eax;
            *((uint32_t*)(cpu_brand + i * 16 + 4)) = ebx;
            *((uint32_t*)(cpu_brand + i * 16 + 8)) = ecx;
            *((uint32_t*)(cpu_brand + i * 16 + 12)) = edx;
        }
        cpu_brand[48] = '\0';
        // Strip leading spaces (memmove: src/dst may overlap)
        char* start = cpu_brand;
        while (*start == ' ') start++;
        if (start != cpu_brand) {
            memmove(cpu_brand, start, strlen(start) + 1);
        }
    } else {
        strncpy(cpu_brand, "Unknown CPU", 48);
        cpu_brand[47] = '\0';
    }
}

void cpu_detect_topology(cpu_info_t* info) {
    uint32_t eax, ebx, ecx, edx;

    if (!info) return;

    detect_cpu_info(info->vendor, info->brand);

    cpu_cpuid(0x00000001, 0, &eax, &ebx, &ecx, &edx);
    info->features_ecx = ecx;
    info->features_edx = edx;

    debug_printf("[CPU] Initial detection: EAX=0x%08x, EBX=0x%08x\n", eax, ebx);

    bool has_htt = (edx & (1 << 28)) != 0;
    debug_printf("[CPU] HTT support: %s\n", has_htt ? "yes" : "no");

    uint8_t max_logical_cores = (ebx >> 16) & 0xFF;
    debug_printf("[CPU] Max logical cores from EBX: %d\n", max_logical_cores);

    cpu_cpuid(0x00000000, 0, &eax, &ebx, &ecx, &edx);
    uint32_t max_cpuid = eax;
    debug_printf("[CPU] Max CPUID function: 0x%08x\n", max_cpuid);

    bool has_extended_topology = (max_cpuid >= 0x0000000B);
    debug_printf("[CPU] Extended topology support: %s\n", has_extended_topology ? "yes" : "no");

    if (has_extended_topology) {
        uint32_t level = 0;
        uint32_t core_bits = 0, thread_bits = 0;

        do {
            cpu_cpuid(0x0000000B, level, &eax, &ebx, &ecx, &edx);
            uint32_t type = (ecx >> 8) & 0xFF;

            debug_printf("[CPU] Topology level %d: type=0x%02x, eax=0x%08x, ebx=0x%08x\n",
                   level, type, eax, ebx);

            if (type == 1) {
                thread_bits = eax & 0x1F;
                debug_printf("[CPU] SMT level: %d bits\n", thread_bits);
            } else if (type == 2) {
                core_bits = eax & 0x1F;
                debug_printf("[CPU] Core level: %d bits\n", core_bits);
            }

            level++;
        } while (eax != 0 && level < 10);

        if (core_bits > 0 && thread_bits > 0) {
            info->threads_per_core = 1 << (thread_bits - core_bits);
            info->logical_cores = ebx & 0xFFFF;
            info->physical_cores = info->logical_cores / info->threads_per_core;
            debug_printf("[CPU] Extended topology: %d physical, %d logical, %d threads/core\n",
                   info->physical_cores, info->logical_cores, info->threads_per_core);
        } else {
            debug_printf("[CPU] Extended topology incomplete, using fallback\n");
            goto fallback_method;
        }
    } else {
fallback_method:
        debug_printf("[CPU] Using fallback detection for QEMU\n");

        cpu_cpuid(0x00000001, 0, &eax, &ebx, &ecx, &edx);
        uint32_t initial_apic_id = (ebx >> 24) & 0xFF;
        debug_printf("[CPU] Initial APIC ID: 0x%02x\n", initial_apic_id);

        if (max_logical_cores > 0) {
            info->logical_cores = max_logical_cores;
        } else {
            info->logical_cores = 1;
        }

        if (has_htt && info->logical_cores > 1) {
            info->threads_per_core = 2;
            info->physical_cores = info->logical_cores / 2;
        } else {
            info->threads_per_core = 1;
            info->physical_cores = info->logical_cores;
        }

        if (info->physical_cores == 0) {
            info->physical_cores = 1;
            info->logical_cores = 1;
            info->threads_per_core = 1;
        }
    }

    cpu_cpuid(0x80000000, 0, &eax, &ebx, &ecx, &edx);
    debug_printf("[CPU] Max extended CPUID: 0x%08x\n", eax);

    if (eax >= 0x80000008) {
        cpu_cpuid(0x80000008, 0, &eax, &ebx, &ecx, &edx);
        uint8_t physical_cores_ext = (ecx & 0xFF) + 1;
        debug_printf("[CPU] Extended core count: %d\n", physical_cores_ext);

        if (physical_cores_ext > info->physical_cores) {
            info->physical_cores = physical_cores_ext;
            info->logical_cores = physical_cores_ext * info->threads_per_core;
        }
    }

    cpu_cpuid(0x80000001, 0, &eax, &ebx, &ecx, &edx);
    info->extended_features_ebx = ebx;
    info->extended_features_ecx = ecx;

    debug_printf("[CPU] Final: %d physical cores, %d logical cores\n",
           info->physical_cores, info->logical_cores);
}

uint8_t cpu_get_core_count(void) {
    if (cpu_info.physical_cores == 0) {
        cpu_detect_topology(&cpu_info);
    }
    return cpu_info.physical_cores;
}

void cpu_print_detailed_info(void) {
    if (cpu_info.physical_cores == 0) {
        cpu_detect_topology(&cpu_info);
    }

    kprintf("\n%[H]=== CPU INFORMATION ===%[D]\n");
    kprintf("Vendor: %s\n", cpu_info.vendor);
    kprintf("Brand:  %s\n", cpu_info.brand);
    kprintf("Physical cores:  %d\n", cpu_info.physical_cores);
    kprintf("Logical cores:   %d\n", cpu_info.logical_cores);
    kprintf("Threads per core: %d\n", cpu_info.threads_per_core);

    kprintf("Features: ");
    if (cpu_info.features_edx & (1 << 28)) kprintf("HT ");
    if (cpu_info.features_ecx & (1 << 0)) kprintf("SSE3 ");
    if (cpu_info.features_ecx & (1 << 9)) kprintf("SSSE3 ");
    if (cpu_info.features_ecx & (1 << 19)) kprintf("SSE4.1 ");
    if (cpu_info.features_ecx & (1 << 20)) kprintf("SSE4.2 ");
    if (cpu_info.features_ecx & (1 << 23)) kprintf("POPCNT ");
    if (cpu_info.features_ecx & (1 << 25)) kprintf("AES ");
    if (cpu_info.features_ecx & (1 << 28)) kprintf("AVX ");
    kprintf("\n");

    kprintf("Extended features: ");
    if (cpu_info.extended_features_ecx & (1 << 5)) kprintf("LZCNT ");
    if (cpu_info.extended_features_ecx & (1 << 6)) kprintf("SSE4A ");
    if (cpu_info.extended_features_ecx & (1 << 29)) kprintf("LM ");  // Long mode
    kprintf("\n");
}
