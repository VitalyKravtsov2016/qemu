#ifndef HW_TIMER_LPC1778_TIMER_H
#define HW_TIMER_LPC1778_TIMER_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "hw/misc/lpc1778_syscon.h"
#include "qom/object.h"

#define TYPE_LPC1778_TIMER "lpc1778-timer"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778TimerState, LPC1778_TIMER)

struct Lpc1778TimerState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;
    Lpc1778SysconState *syscon;

    /* CCLK as wired by the board; PCLK = CCLK / PCLKSEL. */
    uint32_t cclk_hz;
    /* TC steps owed to the counter when the one-shot ptimer fires. */
    uint64_t pending;
    /* Set while the ptimer callback runs: its transaction is already open. */
    bool in_tick;

    uint32_t ir;
    uint32_t ir_raised;
    uint32_t tcr;
    uint32_t tc;
    uint32_t pr;
    uint32_t pc;
    uint32_t mcr;
    uint32_t mr[4];
    uint32_t ccr;
    uint32_t cr[4];
    uint32_t emr;
    uint32_t ctcr;
};

#endif
