/*
 * NXP LPC1778 Windowed Watchdog (UM10470 ch. 31) @ 0x40000000
 *
 * Dedicated watchdog oscillator is 500 kHz (UM10470 3.8.4). Feed sequence
 * 0xAA, 0x55. WDEN/WDRESET/WDPROTECT stick until a chip reset. Setting WDEN
 * does not start the counter — counting begins on the first valid feed, and
 * feed errors are ignored until then. Timeout with WDRESET resets the chip.
 * WDINT is set by the WARNINT match and cleared by writing 1; WDTOF is
 * cleared by writing 0. There is no WDCLKSEL on this part: offset 0x10 is
 * unassigned and the watchdog clock is not selectable.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/lpc1778_wdt.h"
#include "hw/misc/lpc1778_syscon.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/reset.h"
#include "system/runstate.h"

/*
 * UM10470 31.3: the dedicated ~500 kHz watchdog oscillator feeds a fixed
 * divide-by-4 prescaler before the 24-bit down counter.
 */
#define LPC1778_WDT_COUNTER_HZ  (500000 / 4)

#define REG_MOD     0x00
#define REG_TC      0x04
#define REG_FEED    0x08
#define REG_TV      0x0C
#define REG_WARNINT 0x14
#define REG_WINDOW  0x18

#define MOD_WDEN     (1u << 0)
#define MOD_WDRESET  (1u << 1)
#define MOD_WDTOF    (1u << 2)
#define MOD_WDINT    (1u << 3)
#define MOD_WDPROTECT (1u << 4)

#define TC_MIN      0xFFu
#define WARNINT_MAX 0x3FFu

#define FEED_AA     0xAA
#define FEED_55     0x55

/*
 * WDINT is the watchdog interrupt. A timeout in interrupt-only mode
 * (WDRESET = 0) also has to reach the NVIC, since nothing else would.
 */
static void lpc1778_wdt_update_irq(Lpc1778WdtState *s)
{
    bool warn = (s->mod & MOD_WDINT) != 0;
    bool timeout = (s->mod & MOD_WDTOF) && !(s->mod & MOD_WDRESET);

    qemu_set_irq(s->irq, warn || timeout);
}

static void lpc1778_wdt_chip_reset(Lpc1778WdtState *s, const char *why)
{
    error_report("lpc1778-wdt: %s (TC=0x%x)", why, s->tc);
    lpc1778_syscon_note_reset(s->syscon, LPC1778_RSID_WDTR);
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static uint32_t lpc1778_wdt_tc(const Lpc1778WdtState *s)
{
    return s->tc;
}

static uint32_t lpc1778_wdt_tv(Lpc1778WdtState *s)
{
    if (s->counting && s->timer) {
        return (uint32_t)ptimer_get_count(s->timer);
    }
    return lpc1778_wdt_tc(s);
}

/*
 * After 0xAA, any watchdog access other than writing 0x55 to FEED is a feed
 * error and resets the chip. Errors are ignored until the first valid feed
 * has completed after WDEN.
 */
static bool lpc1778_wdt_feed_break(Lpc1778WdtState *s, hwaddr addr,
                                   bool is_write, uint32_t v)
{
    if (s->feed_prev != FEED_AA) {
        return false;
    }
    if (is_write && addr == REG_FEED && (v & 0xFFu) == FEED_55) {
        return false;
    }
    s->feed_prev = 0;
    if ((s->mod & MOD_WDEN) && s->fed_once) {
        s->mod |= MOD_WDTOF;
        lpc1778_wdt_chip_reset(s, "watchdog access inside feed sequence");
    }
    return true;
}

/*
 * UM10470 / Keil: setting WDEN does not start the counter. Counting
 * begins on a valid 0xAA,0x55 feed (and each later feed reloads WDTC).
 */
static void lpc1778_wdt_reload(Lpc1778WdtState *s)
{
    uint32_t tc = lpc1778_wdt_tc(s);
    uint32_t warn = s->warnint & WARNINT_MAX;

    if (!s->timer) {
        return;
    }
    ptimer_transaction_begin(s->timer);
    if (s->mod & MOD_WDEN) {
        ptimer_set_limit(s->timer, tc, 1);
        ptimer_run(s->timer, 1);
        s->counting = true;
    } else {
        ptimer_stop(s->timer);
        s->counting = false;
    }
    ptimer_transaction_commit(s->timer);

    if (!s->warn_timer) {
        return;
    }
    ptimer_transaction_begin(s->warn_timer);
    if (s->counting && warn && warn < tc) {
        /* WDINT fires when the down counter reaches WARNINT. */
        ptimer_set_limit(s->warn_timer, tc - warn, 1);
        ptimer_run(s->warn_timer, 1);
    } else {
        ptimer_stop(s->warn_timer);
    }
    ptimer_transaction_commit(s->warn_timer);
}

static void lpc1778_wdt_stop(Lpc1778WdtState *s)
{
    s->counting = false;
    if (s->timer) {
        ptimer_transaction_begin(s->timer);
        ptimer_stop(s->timer);
        ptimer_transaction_commit(s->timer);
    }
    if (s->warn_timer) {
        ptimer_transaction_begin(s->warn_timer);
        ptimer_stop(s->warn_timer);
        ptimer_transaction_commit(s->warn_timer);
    }
}

static void lpc1778_wdt_warn_tick(void *opaque)
{
    Lpc1778WdtState *s = opaque;

    s->mod |= MOD_WDINT;
    lpc1778_wdt_update_irq(s);
}

static void lpc1778_wdt_tick(void *opaque)
{
    Lpc1778WdtState *s = opaque;

    s->mod |= MOD_WDTOF;
    s->counting = false;
    lpc1778_wdt_update_irq(s);
    if (s->mod & MOD_WDRESET) {
        lpc1778_wdt_chip_reset(s, "timeout, chip reset");
        return;
    }
    /* Interrupt-only: keep counting so the next timeout can still fire. */
    lpc1778_wdt_reload(s);
}

static uint64_t lpc1778_wdt_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778WdtState *s = opaque;

    (void)size;
    lpc1778_wdt_feed_break(s, addr, false, 0);
    switch (addr) {
    case REG_MOD:
        return s->mod;
    case REG_TC:
        return s->tc;
    case REG_TV:
        return lpc1778_wdt_tv(s);
    case REG_WARNINT:
        return s->warnint;
    case REG_WINDOW:
        return s->window;
    default:
        return 0;
    }
}

static bool lpc1778_wdt_feed_in_window(Lpc1778WdtState *s)
{
    if (s->window >= 0xFFFFFFu) {
        return true;
    }
    /* A feed before the counter has dropped to WINDOW is a watchdog event. */
    return lpc1778_wdt_tv(s) <= s->window;
}

static void lpc1778_wdt_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778WdtState *s = opaque;
    uint32_t v = (uint32_t)value;
    bool feed_ok = s->feed_prev == FEED_AA;

    (void)size;
    if (lpc1778_wdt_feed_break(s, addr, true, v)) {
        feed_ok = false;
    }
    switch (addr) {
    case REG_MOD:
        /*
         * WDEN/WDRESET/WDPROTECT are set-only and stick until a chip reset.
         * Software cannot raise the flags: WDTOF is cleared by writing 0,
         * WDINT by writing 1.
         */
        s->mod |= v & (MOD_WDEN | MOD_WDRESET | MOD_WDPROTECT);
        if (!(v & MOD_WDTOF)) {
            s->mod &= ~MOD_WDTOF;
        }
        if (v & MOD_WDINT) {
            s->mod &= ~MOD_WDINT;
        }
        lpc1778_wdt_update_irq(s);
        break;
    case REG_TC:
        /*
         * With WDPROTECT set, TC may only change once the counter is below
         * both WARNINT and WINDOW; otherwise it is a watchdog event.
         */
        if (s->mod & MOD_WDPROTECT) {
            uint32_t tv = lpc1778_wdt_tv(s);

            if (tv >= (s->warnint & WARNINT_MAX) || tv >= s->window) {
                s->mod |= MOD_WDTOF;
                lpc1778_wdt_chip_reset(s, "WDTC write under WDPROTECT");
                break;
            }
        }
        /* Values below 0xFF load 0xFF, so the readback shows the floor. */
        s->tc = v < TC_MIN ? TC_MIN : v;
        break;
    case REG_FEED:
        v &= 0xFFu;
        if (feed_ok && v == FEED_55) {
            s->feed_prev = 0;
            if (!(s->mod & MOD_WDEN)) {
                break;
            }
            if (s->fed_once && !lpc1778_wdt_feed_in_window(s)) {
                s->mod |= MOD_WDTOF;
                lpc1778_wdt_chip_reset(s, "feed outside window, chip reset");
                break;
            }
            s->fed_once = true;
            lpc1778_wdt_reload(s);
        } else if (v == FEED_AA) {
            s->feed_prev = FEED_AA;
        }
        break;
    case REG_WARNINT:
        s->warnint = v & WARNINT_MAX;
        break;
    case REG_WINDOW:
        s->window = v;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps lpc1778_wdt_ops = {
    .read = lpc1778_wdt_read,
    .write = lpc1778_wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_wdt_reset(DeviceState *dev)
{
    Lpc1778WdtState *s = LPC1778_WDT(dev);

    s->mod = 0;
    s->tc = TC_MIN;
    s->feed_prev = 0;
    s->fed_once = false;
    s->warnint = 0;
    s->window = 0xFFFFFF;
    s->counting = false;
    lpc1778_wdt_update_irq(s);
    lpc1778_wdt_stop(s);
}

static void lpc1778_wdt_init(Object *obj)
{
    Lpc1778WdtState *s = LPC1778_WDT(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_wdt_ops, s,
                          TYPE_LPC1778_WDT, 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_wdt_realize(DeviceState *dev, Error **errp)
{
    Lpc1778WdtState *s = LPC1778_WDT(dev);

    (void)errp;
    s->timer = ptimer_init(lpc1778_wdt_tick, s, PTIMER_POLICY_LEGACY);
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, LPC1778_WDT_COUNTER_HZ);
    ptimer_transaction_commit(s->timer);

    s->warn_timer = ptimer_init(lpc1778_wdt_warn_tick, s, PTIMER_POLICY_LEGACY);
    ptimer_transaction_begin(s->warn_timer);
    ptimer_set_freq(s->warn_timer, LPC1778_WDT_COUNTER_HZ);
    ptimer_transaction_commit(s->warn_timer);
}

static void lpc1778_wdt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    (void)data;
    device_class_set_legacy_reset(dc, lpc1778_wdt_reset);
    dc->realize = lpc1778_wdt_realize;
}

static const TypeInfo lpc1778_wdt_info = {
    .name = TYPE_LPC1778_WDT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778WdtState),
    .instance_init = lpc1778_wdt_init,
    .class_init = lpc1778_wdt_class_init,
};

static void lpc1778_wdt_register_types(void)
{
    type_register_static(&lpc1778_wdt_info);
}

type_init(lpc1778_wdt_register_types)
