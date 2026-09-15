#ifndef HW_MISC_LPC1778_GPDMA_H
#define HW_MISC_LPC1778_GPDMA_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_LPC1778_GPDMA "lpc1778-gpdma"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778GpdmaState, LPC1778_GPDMA)

struct Lpc1778GpdmaChan {
    uint32_t src;
    uint32_t dst;
    uint32_t lli;
    uint32_t control;
    uint32_t config;
};

struct Lpc1778GpdmaState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t config;
    uint32_t sync;
    uint32_t int_tc;
    uint32_t int_err;
    uint32_t raw_tc;
    uint32_t raw_err;
    uint32_t soft_breq;
    uint32_t soft_sreq;
    uint32_t soft_lbreq;
    uint32_t soft_lsreq;
    struct Lpc1778GpdmaChan ch[8];
};

#endif
