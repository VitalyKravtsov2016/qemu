#ifndef HW_TIMER_LPC1778_PWM_H
#define HW_TIMER_LPC1778_PWM_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "hw/misc/lpc1778_syscon.h"
#include "qom/object.h"

#define TYPE_LPC1778_PWM "lpc1778-pwm"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778PwmState, LPC1778_PWM)

struct Lpc1778PwmState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;
    Lpc1778SysconState *syscon;

    /* CCLK as wired by the board; PCLK = CCLK / PCLKSEL. */
    uint32_t cclk_hz;
    /* TC steps still owed to the counter when the one-shot ptimer fires. */
    uint32_t pending;
    /* Set while the ptimer callback runs: its transaction is already open. */
    bool in_tick;

    uint32_t ir;
    uint32_t tcr;
    uint32_t tc;
    uint32_t pr;
    uint32_t pc;
    uint32_t mcr;
    uint32_t mr[7];
    uint32_t shadow[7];
    uint32_t pcr;
    uint32_t ler;
    uint32_t ctcr;

    /*
     * Optional board hook:
     *   channel 0     — period reset, single-edge PWM outputs rise (UM10470)
     *   channel 1..6  — MRn match, that PWM output falls if PWMENAn
     */
    void (*match_cb)(void *opaque, int channel);
    void *match_opaque;
};

#endif
