#ifndef HW_SSI_LPC1778_SSP_H
#define HW_SSI_LPC1778_SSP_H

#include "hw/core/sysbus.h"
#include "hw/ssi/ssi.h"
#include "qom/object.h"

#define TYPE_LPC1778_SSP "lpc1778-ssp"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778SspState, LPC1778_SSP)

#define LPC1778_SSP_FIFO 8

struct Lpc1778SspState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;

    uint32_t cr0;
    uint32_t cr1;
    uint32_t cpsr;
    uint32_t imsc;
    uint32_t ris;
    uint32_t dmacr;

    uint16_t rx_fifo[LPC1778_SSP_FIFO];
    int rx_head;
    int rx_tail;
    int rx_len;

    qemu_irq irq;
    SSIBus *ssi;
};

/* Data size in bits, CR0.DSS + 1 (UM10470 21.6.1). */
unsigned lpc1778_ssp_word_bits(const Lpc1778SspState *s);

#endif
