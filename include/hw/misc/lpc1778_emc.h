#ifndef HW_MISC_LPC1778_EMC_H
#define HW_MISC_LPC1778_EMC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_LPC1778_EMC "lpc1778-emc"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778EmcState, LPC1778_EMC)

/* StaticConfig0 lives at offset 0x200; CS0..CS3 occupy 0x200 + n*0x20. */
#define LPC1778_EMC_SIZE 0x400

struct Lpc1778EmcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[LPC1778_EMC_SIZE / 4];
};

#endif
