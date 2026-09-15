#ifndef HW_GPIO_LPC1778_GPIO_H
#define HW_GPIO_LPC1778_GPIO_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_LPC1778_GPIO "lpc1778-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778GpioState, LPC1778_GPIO)

#define LPC1778_GPIO_PORTS 6
#define LPC1778_GPIO_MAX_CHG 4

struct Lpc1778GpioState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t dir[LPC1778_GPIO_PORTS];
    uint32_t mask[LPC1778_GPIO_PORTS];
    uint32_t out[LPC1778_GPIO_PORTS]; /* output latch */
    uint32_t in[LPC1778_GPIO_PORTS];  /* external input levels */

    /* Host-facing cabinet sensors (QMP / UI). Persist across device reset. */
    bool paper_present;
    bool paper_near_end;
    bool cover_closed;
    bool feed_pressed;
    bool drawer_closed;

    void (*change[LPC1778_GPIO_MAX_CHG])(void *opaque);
    void *change_opaque[LPC1778_GPIO_MAX_CHG];
    int nchange;
};

uint32_t lpc1778_gpio_get_pins(const Lpc1778GpioState *s, int port);
void lpc1778_gpio_add_change_handler(Lpc1778GpioState *s,
                                     void (*fn)(void *), void *opaque);
void lpc1778_gpio_set_in_bit(Lpc1778GpioState *s, unsigned port,
                             unsigned bit, bool high);
void lpc1778_gpio_apply_board(Lpc1778GpioState *s);

#endif
