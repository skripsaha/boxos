
#ifndef MCE_H
#define MCE_H

#include "ktypes.h"
#include "error.h"
#include "idt.h"


#define MCE_MSR_MCG_CAP              0x179U
#define MCE_MSR_MCG_STATUS           0x17AU
#define MCE_MSR_MCG_CTL              0x17BU
#define MCE_MSR_MC_CTL_BASE          0x400U
#define MCE_MSR_MC_STATUS_BASE       0x401U
#define MCE_MSR_MC_ADDR_BASE         0x402U
#define MCE_MSR_MC_MISC_BASE         0x403U
#define MCE_MSR_MC_CTL2_BASE         0x280U

#define MCE_MSR_IA32_FEATURE_CONTROL 0x3AU
#define MCE_MSR_FEAT_LOCK_BIT        (1ULL << 0)
#define MCE_MSR_FEAT_LMCE_BIT        (1ULL << 20)


#define MCE_CAP_COUNT_MASK           0xFFU
#define MCE_CAP_MCG_CTL_P            (1ULL <<  8)
#define MCE_CAP_MCG_EXT_P            (1ULL <<  9)
#define MCE_CAP_MCG_CMCI_P           (1ULL << 10)
#define MCE_CAP_MCG_TES_P            (1ULL << 11)
#define MCE_CAP_MCG_SER_P            (1ULL << 24)
#define MCE_CAP_MCG_LMCE_P           (1ULL << 27)


#define MCE_MCG_STATUS_RIPV          (1ULL << 0)
#define MCE_MCG_STATUS_EIPV          (1ULL << 1)
#define MCE_MCG_STATUS_MCIP          (1ULL << 2)
#define MCE_MCG_STATUS_LMCE_S        (1ULL << 3)


#define MCE_MC_STATUS_VAL            (1ULL << 63)
#define MCE_MC_STATUS_OVER           (1ULL << 62)
#define MCE_MC_STATUS_UC             (1ULL << 61)
#define MCE_MC_STATUS_EN             (1ULL << 60)
#define MCE_MC_STATUS_MISCV          (1ULL << 59)
#define MCE_MC_STATUS_ADDRV          (1ULL << 58)
#define MCE_MC_STATUS_PCC            (1ULL << 57)
#define MCE_MC_STATUS_S              (1ULL << 56)
#define MCE_MC_STATUS_AR             (1ULL << 55)

#define MCE_MAX_BANKS                64u


typedef enum {
    MCE_SEV_NONE      = 0,
    MCE_SEV_CORRECTED = 1,
    MCE_SEV_UCR       = 2,
    MCE_SEV_UC        = 3,
} mce_severity_t;


void          mce_init(void);

void          mce_ap_init(void);

bool          mce_handle(interrupt_frame_t *frame);

bool          mce_is_initialized(void);

uint32_t      mce_bank_count(void);

bool          mce_lmce_enabled(void);

#endif