#ifndef HW_MISC_LPC1778_WDT_H
#define HW_MISC_LPC1778_WDT_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"

#define TYPE_LPC1778_WDT "lpc1778-wdt"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778WdtState, LPC1778_WDT)

struct Lpc1778SysconState;

struct Lpc1778WdtState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;
    /* Fires at the WARNINT match, ahead of the timeout. */
    ptimer_state *warn_timer;
    struct Lpc1778SysconState *syscon;

    uint32_t mod;
    uint32_t tc;
    uint32_t feed_prev;
    uint32_t warnint;
    uint32_t window;
    bool counting;
    /* Feed errors are ignored until the first valid feed after WDEN. */
    bool fed_once;
};

#endif
