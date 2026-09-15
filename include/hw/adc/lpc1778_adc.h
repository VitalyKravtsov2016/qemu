#ifndef HW_ADC_LPC1778_ADC_H
#define HW_ADC_LPC1778_ADC_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"

#define TYPE_LPC1778_ADC "lpc1778-adc"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778AdcState, LPC1778_ADC)

struct Lpc1778GpioState;

struct Lpc1778AdcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t cr;
    uint32_t gdr;
    uint32_t inten;
    uint32_t trm;
    uint32_t dr[8];
    qemu_irq irq;
    ptimer_state *burst;
    unsigned burst_ch;
    /* Not owned. Paper optics AD0[6]/AD0[7] follow GPIO sensor QOM props. */
    struct Lpc1778GpioState *gpio;
};

#endif
