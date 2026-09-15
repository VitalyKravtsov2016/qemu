#ifndef HW_MISC_LPC1778_IOCON_H
#define HW_MISC_LPC1778_IOCON_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_LPC1778_IOCON "lpc1778-iocon"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778IoconState, LPC1778_IOCON)

#define LPC1778_IOCON_SIZE 0x400

struct Lpc1778IoconState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[LPC1778_IOCON_SIZE / 4];
};

#endif
