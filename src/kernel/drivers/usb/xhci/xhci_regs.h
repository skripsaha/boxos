#ifndef XHCI_REGS_H
#define XHCI_REGS_H

#include "ktypes.h"

typedef struct {
    volatile uint32_t hc_capbase;
    volatile uint32_t hcsparams1;
    volatile uint32_t hcsparams2;
    volatile uint32_t hcsparams3;
    volatile uint32_t hccparams1;
    volatile uint32_t dboff;
    volatile uint32_t rtsoff;
    volatile uint32_t hccparams2;
} xhci_cap_regs_t;

#define XHCI_CAP_LENGTH(p)       ((p) & 0xFF)
#define XHCI_CAP_VERSION(p)      (((p) >> 16) & 0xFFFF)

#define XHCI_HCS1_MAX_SLOTS(p)   ((p) & 0xFF)
#define XHCI_HCS1_MAX_INTRS(p)   (((p) >> 8) & 0x7FF)
#define XHCI_HCS1_MAX_PORTS(p)   (((p) >> 24) & 0xFF)

#define XHCI_HCS2_IST(p)             ((p) & 0xF)
#define XHCI_HCS2_ERST_MAX(p)        (((p) >> 4) & 0xF)
#define XHCI_HCS2_MAX_SCRATCHPAD(p)  ((((p) >> 21) & 0x1F) << 5 | (((p) >> 27) & 0x1F))

#define XHCI_HCC1_AC64           (1u << 0)
#define XHCI_HCC1_CSZ            (1u << 2)
#define XHCI_HCC1_PPC            (1u << 3)
#define XHCI_HCC1_EXT_CAPS(p)    (((p) >> 16) & 0xFFFF)

#define XHCI_ECAP_ID(d)          ((d) & 0xFF)
#define XHCI_ECAP_NEXT(d)        (((d) >> 8) & 0xFF)
#define XHCI_ECAP_ID_LEGACY      1
#define XHCI_ECAP_ID_PROTOCOL    2

#define XHCI_LEGSUP_BIOS_OWNED   (1u << 16)
#define XHCI_LEGSUP_OS_OWNED     (1u << 24)

#define XHCI_LEGCTL_RSVDP        ((0x7u << 1) | (0xFFu << 5) | (0x7u << 17))
#define XHCI_LEGCTL_SMI_W1C      (0x7u << 29)

#define XHCI_PROTO_MINOR(d)      (((d) >> 16) & 0xFF)
#define XHCI_PROTO_MAJOR(d)      (((d) >> 24) & 0xFF)
#define XHCI_PROTO_PORT_OFF(d)   ((d) & 0xFF)
#define XHCI_PROTO_PORT_COUNT(d) (((d) >> 8) & 0xFF)
#define XHCI_PROTO_PSIC(d)       (((d) >> 28) & 0xF)
#define XHCI_PROTO_SLOT_TYPE(d)  ((d) & 0x1F)
#define XHCI_PROTO_NAME_USB      0x20425355u

typedef struct {
    volatile uint32_t usbcmd;
    volatile uint32_t usbsts;
    volatile uint32_t pagesize;
    uint32_t reserved1[2];
    volatile uint32_t dnctrl;
    volatile uint64_t crcr;
    uint32_t reserved2[4];
    volatile uint64_t dcbaap;
    volatile uint32_t config;
} xhci_op_regs_t;

#define XHCI_CMD_RUN             (1 << 0)
#define XHCI_CMD_RESET           (1 << 1)
#define XHCI_CMD_INTE            (1 << 2)
#define XHCI_CMD_HSEE            (1 << 3)

#define XHCI_STS_HCH             (1 << 0)
#define XHCI_STS_HSE             (1 << 2)
#define XHCI_STS_EINT            (1 << 3)
#define XHCI_STS_PCD             (1 << 4)
#define XHCI_STS_CNR             (1 << 11)
#define XHCI_STS_HCE             (1 << 12)

#define XHCI_CRCR_RCS            (1 << 0)
#define XHCI_CRCR_CS             (1 << 1)
#define XHCI_CRCR_CA             (1 << 2)
#define XHCI_CRCR_CRR            (1 << 3)

typedef struct {
    volatile uint32_t portsc;
    volatile uint32_t portpmsc;
    volatile uint32_t portli;
    volatile uint32_t porthlpmc;
} xhci_port_regs_t;

#define XHCI_PORTSC_CCS          (1 << 0)
#define XHCI_PORTSC_PED          (1 << 1)
#define XHCI_PORTSC_OCA          (1 << 3)
#define XHCI_PORTSC_PR           (1 << 4)
#define XHCI_PORTSC_PLS_MASK     (0xF << 5)
#define XHCI_PORTSC_PP           (1 << 9)
#define XHCI_PORTSC_SPEED_MASK   (0xF << 10)
#define XHCI_PORTSC_LWS          (1 << 16)
#define XHCI_PORTSC_CSC          (1 << 17)
#define XHCI_PORTSC_PEC          (1 << 18)
#define XHCI_PORTSC_WRC          (1 << 19)
#define XHCI_PORTSC_OCC          (1 << 20)
#define XHCI_PORTSC_PRC          (1 << 21)
#define XHCI_PORTSC_PLC          (1 << 22)
#define XHCI_PORTSC_CEC          (1 << 23)
#define XHCI_PORTSC_WPR          (1u << 31)

#define XHCI_PORTSC_PLS_SHIFT    5
#define XHCI_PORTSC_PLS(p)       (((p) >> XHCI_PORTSC_PLS_SHIFT) & 0xF)
#define XHCI_PLS_U0              0
#define XHCI_PLS_DISABLED        4
#define XHCI_PLS_RXDETECT        5
#define XHCI_PLS_INACTIVE        6
#define XHCI_PLS_POLLING         7
#define XHCI_PLS_COMPLIANCE      10

#define XHCI_PORTSC_SPEED_SHIFT  10
#define XHCI_PORTSC_SPEED(p)     (((p) >> XHCI_PORTSC_SPEED_SHIFT) & 0xF)

#define XHCI_PORTSC_W1C_MASK     (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | \
                                  XHCI_PORTSC_WRC | XHCI_PORTSC_OCC | \
                                  XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | \
                                  XHCI_PORTSC_CEC)

#define XHCI_PORTSC_RMW_CLEAR    (XHCI_PORTSC_W1C_MASK | XHCI_PORTSC_PED | \
                                  XHCI_PORTSC_PR | XHCI_PORTSC_WPR | \
                                  XHCI_PORTSC_LWS)

typedef struct {
    volatile uint32_t iman;
    volatile uint32_t imod;
    volatile uint32_t erstsz;
    uint32_t reserved;
    volatile uint64_t erstba;
    volatile uint64_t erdp;
} xhci_interrupter_regs_t;

#define XHCI_IMAN_IP             (1 << 0)
#define XHCI_IMAN_IE             (1 << 1)

#define XHCI_IMOD_DEFAULT        4000

#define XHCI_ERDP_DESI_MASK      0x7
#define XHCI_ERDP_EHB            (1 << 3)

typedef struct {
    volatile uint32_t mfindex;
    uint32_t reserved[7];
    xhci_interrupter_regs_t interrupters[1024];
} xhci_runtime_regs_t;

typedef struct {
    volatile uint32_t doorbell;
} xhci_doorbell_t;

typedef struct {
    xhci_doorbell_t doorbells[256];
} xhci_doorbell_array_t;

_Static_assert(sizeof(xhci_cap_regs_t) == 0x20, "capability registers are 32 bytes");
_Static_assert(__builtin_offsetof(xhci_cap_regs_t, hcsparams1) == 0x04, "HCSPARAMS1 at 0x04");
_Static_assert(__builtin_offsetof(xhci_cap_regs_t, hcsparams2) == 0x08, "HCSPARAMS2 at 0x08");
_Static_assert(__builtin_offsetof(xhci_cap_regs_t, hccparams1) == 0x10, "HCCPARAMS1 at 0x10");
_Static_assert(__builtin_offsetof(xhci_cap_regs_t, dboff)      == 0x14, "DBOFF at 0x14");
_Static_assert(__builtin_offsetof(xhci_cap_regs_t, rtsoff)     == 0x18, "RTSOFF at 0x18");

_Static_assert(__builtin_offsetof(xhci_op_regs_t, usbsts)   == 0x04, "USBSTS at 0x04");
_Static_assert(__builtin_offsetof(xhci_op_regs_t, pagesize) == 0x08, "PAGESIZE at 0x08");
_Static_assert(__builtin_offsetof(xhci_op_regs_t, dnctrl)   == 0x14, "DNCTRL at 0x14");
_Static_assert(__builtin_offsetof(xhci_op_regs_t, crcr)     == 0x18, "CRCR at 0x18");
_Static_assert(__builtin_offsetof(xhci_op_regs_t, dcbaap)   == 0x30, "DCBAAP at 0x30");
_Static_assert(__builtin_offsetof(xhci_op_regs_t, config)   == 0x38, "CONFIG at 0x38");

_Static_assert(sizeof(xhci_port_regs_t) == 0x10, "a port register set is 16 bytes");

_Static_assert(sizeof(xhci_interrupter_regs_t) == 0x20, "an interrupter set is 32 bytes");
_Static_assert(__builtin_offsetof(xhci_interrupter_regs_t, erstsz) == 0x08, "ERSTSZ at 0x08");
_Static_assert(__builtin_offsetof(xhci_interrupter_regs_t, erstba) == 0x10, "ERSTBA at 0x10");
_Static_assert(__builtin_offsetof(xhci_interrupter_regs_t, erdp)   == 0x18, "ERDP at 0x18");

_Static_assert(__builtin_offsetof(xhci_runtime_regs_t, interrupters) == 0x20,
               "the interrupter array starts at RTSOFF + 0x20");

_Static_assert(sizeof(xhci_doorbell_t) == 4, "a doorbell is one dword");

#endif