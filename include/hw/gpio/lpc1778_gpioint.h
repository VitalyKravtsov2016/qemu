#ifndef HW_GPIO_LPC1778_GPIOINT_H
#define HW_GPIO_LPC1778_GPIOINT_H

#include "hw/core/sysbus.h"
#include "hw/gpio/lpc1778_gpio.h"
#include "qom/object.h"

#define TYPE_LPC1778_GPIOINT "lpc1778-gpioint"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778GpioIntState, LPC1778_GPIOINT)

struct Lpc1778GpioIntState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    Lpc1778GpioState *gpio;

    uint32_t last[2]; /* P0, P2 */
    uint32_t en_r[2];
    uint32_t en_f[2];
    uint32_t stat_r[2];
    uint32_t stat_f[2];
};

#endif
