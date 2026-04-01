#include "../tagfs.h"
#include "../../lib/kernel/klib.h"

// ============================================================================
// Self-Healing — Production Stub
// Note: Full implementation requires metadata mirroring and scrub daemon
// ============================================================================

static bool g_heal_enabled = false;

void TagFS_SelfHealInit(void) {
    g_heal_enabled = false;
    debug_printf("[TagFS Self-Heal] Disabled (requires metadata mirroring)\n");
}

void TagFS_SelfHealShutdown(void) {
    g_heal_enabled = false;
}

void TagFS_SelfHealEnable(bool enable) {
    g_heal_enabled = enable;
    debug_printf("[TagFS Self-Heal] %s\n", enable ? "enabled" : "disabled");
}

void TagFS_SelfHealOnMetadataWrite(uint32_t block_number, const uint8_t* data) {
    (void)block_number;
    (void)data;
}

int TagFS_SelfHealOnMetadataRead(uint32_t block_number, uint8_t* data) {
    (void)block_number;
    (void)data;
    return OK;
}

void TagFS_SelfHealPeriodicCheck(void) {
}
