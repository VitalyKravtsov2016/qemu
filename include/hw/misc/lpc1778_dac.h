#ifndef HW_MISC_LPC1778_DAC_H
#define HW_MISC_LPC1778_DAC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_LPC1778_DAC "lpc1778-dac"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778DacState, LPC1778_DAC)

struct Lpc1778DacState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t dacr;
    uint32_t ctrl;
    uint32_t cntval;
};

#endif
