#ifndef XHCI_REGS_H
#define XHCI_REGS_H

#include "ktypes.h"

typedef struct {
    uint8_t caplength;
    uint8_t reserved;
    uint16_t hciversion;
    uint32_t hcsparams1;
    uint32_t hcsparams2;
    uint32_t hcsparams3;
    uint32_t hccparams1;
    uint32_t dboff;
    uint32_t rtsoff;
    uint32_t hccparams2;
} __attribute__((packed)) xhci_cap_regs_t;

#define XHCI_HCS1_MAX_SLOTS(p)   ((p) & 0xFF)
#define XHCI_HCS1_MAX_INTRS(p)   (((p) >> 8) & 0x7FF)
#define XHCI_HCS1_MAX_PORTS(p)   (((p) >> 24) & 0xFF)

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
} __attribute__((packed)) xhci_op_regs_t;

#define XHCI_CMD_RUN             (1 << 0)
#define XHCI_CMD_RESET           (1 << 1)
#define XHCI_CMD_INTE            (1 << 2)
#define XHCI_CMD_HSEE            (1 << 3)

#define XHCI_STS_HCH             (1 << 0)
#define XHCI_STS_HSE             (1 << 2)
#define XHCI_STS_EINT            (1 << 3)
#define XHCI_STS_PCD             (1 << 4)
#define XHCI_STS_CNR             (1 << 11)

#define XHCI_CRCR_RCS            (1 << 0)
#define XHCI_CRCR_CS             (1 << 1)
#define XHCI_CRCR_CA             (1 << 2)
#define XHCI_CRCR_CRR            (1 << 3)

typedef struct {
    volatile uint32_t portsc;
    volatile uint32_t portpmsc;
    volatile uint32_t portli;
    volatile uint32_t porthlpmc;
} __attribute__((packed)) xhci_port_regs_t;

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

#define XHCI_PORTSC_W1C_MASK     (XHCI_PORTSC_CSC | XHCI_PORTSC_PEC | \
                                  XHCI_PORTSC_WRC | XHCI_PORTSC_OCC | \
                                  XHCI_PORTSC_PRC | XHCI_PORTSC_PLC | \
                                  XHCI_PORTSC_CEC)

typedef struct {
    volatile uint32_t iman;
    volatile uint32_t imod;
    volatile uint32_t erstsz;
    uint32_t reserved;
    volatile uint64_t erstba;
    volatile uint64_t erdp;
} __attribute__((packed)) xhci_interrupter_regs_t;

#define XHCI_IMAN_IP             (1 << 0)
#define XHCI_IMAN_IE             (1 << 1)

#define XHCI_ERDP_DESI_MASK      0x7
#define XHCI_ERDP_EHB            (1 << 3)

typedef struct {
    volatile uint32_t mfindex;
    uint32_t reserved[7];
    xhci_interrupter_regs_t interrupters[1024];
} __attribute__((packed)) xhci_runtime_regs_t;

typedef struct {
    volatile uint32_t doorbell;
} __attribute__((packed)) xhci_doorbell_t;

typedef struct {
    xhci_doorbell_t doorbells[256];
} __attribute__((packed)) xhci_doorbell_array_t;

#endif
