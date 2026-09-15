#ifndef HW_MISC_LPC1778_RTC_H
#define HW_MISC_LPC1778_RTC_H

#include "hw/core/sysbus.h"
#include "qemu/timer.h"
#include "qom/object.h"
#include <time.h>

#define TYPE_LPC1778_RTC "lpc1778-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778RtcState, LPC1778_RTC)

#define LPC1778_RTC_SIZE 0x400

struct Lpc1778RtcState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    QEMUTimer *tick;
    char *persist_path;
    unsigned save_div;

    uint32_t ilr;
    uint32_t ccr;
    uint32_t ciir;
    uint32_t amr;
    uint32_t cal;
    uint32_t aux;
    uint32_t auxen;
    uint32_t gpreg[5];
    uint32_t alarm[8];

    /* Guest calendar as Unix seconds (UTC fields as written by firmware). */
    int64_t guest_sec;
    /* Host Unix seconds when guest_sec was last latched. */
    int64_t host_sec;
};

#endif
