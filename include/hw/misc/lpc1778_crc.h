#ifndef HW_MISC_LPC1778_CRC_H
#define HW_MISC_LPC1778_CRC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_LPC1778_CRC "lpc1778-crc"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778CrcState, LPC1778_CRC)

struct Lpc1778CrcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t mode;
    uint32_t seed;
    uint32_t crc;
};

#endif
