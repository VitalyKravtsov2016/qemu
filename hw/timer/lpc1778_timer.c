/*
 * NXP LPC1778 Timer 0/1/2/3 (UM10470 ch. 24).
 *
 * TIMER3 @ 0x40094000 (IRQ 4) is the paper-feed/mech tick: firmware sets
 * MCR = 0x600 (MR3 interrupt + reset) then CEN. MR3 often stays 0, so a
 * match must be seen when TC is 0 after CRST/CEN — not only after TC++.
 *
 * TC advances at PCLK/(PR+1) — tens of MHz — so the ptimer is not ticked per
 * count: it is armed for the distance to the nearest armed match, exactly as
 * the PWM model does. TC/PC reads are derived from the running ptimer, which
 * is what a firmware delay loop polling TC needs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/timer/lpc1778_timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define REG_IR   0x00
#define REG_TCR  0x04
#define REG_TC   0x08
#define REG_PR   0x0C
#define REG_PC   0x10
#define REG_MCR  0x14
#define REG_MR0  0x18
#define REG_CCR  0x28
#define REG_CR0  0x2C
#define REG_EMR  0x3C
#define REG_CTCR 0x70

#define TCR_CEN  (1u << 0)
#define TCR_CRST (1u << 1)

static void lpc1778_timer_update_irq(Lpc1778TimerState *s)
{
    uint32_t fresh = s->ir & ~s->ir_raised;

    /*
     * Pulse only newly set IR bits. A level line + ISR that clears the wrong
     * match (type-4 abort writes IR=1 while MR3 is pending) storms IRQ 2 and
     * the guest never replies to 10h/11h, then SYSRESETREQ. Silicon is level
     * triggered; one pulse per match is enough for a correctly written ISR.
     */
    if (fresh) {
        qemu_set_irq(s->irq, 1);
        qemu_set_irq(s->irq, 0);
        s->ir_raised = s->ir;
    }
    if (!s->ir) {
        s->ir_raised = 0;
        qemu_set_irq(s->irq, 0);
    }
}

static void lpc1778_timer_emr_match(Lpc1778TimerState *s, int ch)
{
    uint32_t act = (s->emr >> (4 + 2 * ch)) & 3u;
    uint32_t bit = 1u << ch;

    switch (act) {
    case 1:
        s->emr &= ~bit;
        break;
    case 2:
        s->emr |= bit;
        break;
    case 3:
        s->emr ^= bit;
        break;
    default:
        break;
    }
}

static void lpc1778_timer_check_matches(Lpc1778TimerState *s)
{
    uint32_t now = s->tc;
    bool reset = false;
    int i;

    for (i = 0; i < 4; i++) {
        uint32_t mcr_shift = (uint32_t)i * 3u;

        if (now != s->mr[i]) {
            continue;
        }
        if (s->mcr & (1u << mcr_shift)) {
            s->ir |= 1u << i;
        }
        lpc1778_timer_emr_match(s, i);
        /* All matches see the pre-reset TC, so collect the reset and apply
         * it once the whole compare pass is done. */
        if (s->mcr & (1u << (mcr_shift + 1))) {
            reset = true;
        }
        if (s->mcr & (1u << (mcr_shift + 2))) {
            s->tcr &= ~TCR_CEN;
        }
    }
    if (reset) {
        s->tc = 0;
        s->pc = 0;
    }
    lpc1778_timer_update_irq(s);
}

/* UM10470 24.6.3: TC increments at PCLK / (PR + 1). */
static uint32_t lpc1778_timer_tc_rate(const Lpc1778TimerState *s)
{
    uint32_t pclk = lpc1778_syscon_pclk_hz(s->syscon, s->cclk_hz);

    return pclk / (s->pr + 1u);
}

static bool lpc1778_timer_running(const Lpc1778TimerState *s)
{
    return (s->tcr & TCR_CEN) && !(s->tcr & TCR_CRST);
}

/* A match matters if MCR arms an action or EMR drives the match output. */
static bool lpc1778_timer_match_armed(const Lpc1778TimerState *s, int i)
{
    if (s->mcr & (7u << ((uint32_t)i * 3u))) {
        return true;
    }
    return ((s->emr >> (4 + 2 * i)) & 3u) != 0;
}

/* Counts to the nearest armed match, or to the 32-bit wrap if none is. */
static uint64_t lpc1778_timer_next_delta(const Lpc1778TimerState *s)
{
    uint64_t best = (1ull << 32) - s->tc;
    int i;

    for (i = 0; i < 4; i++) {
        if (s->mr[i] <= s->tc || !lpc1778_timer_match_armed(s, i)) {
            continue;
        }
        if (s->mr[i] - s->tc < best) {
            best = s->mr[i] - s->tc;
        }
    }
    return best;
}

/* Counts already elapsed on the armed ptimer. */
static uint64_t lpc1778_timer_elapsed(const Lpc1778TimerState *s)
{
    uint64_t left;

    if (!s->timer || !s->pending) {
        return 0;
    }
    left = ptimer_get_count(s->timer);
    return left < s->pending ? s->pending - left : s->pending;
}

static void lpc1778_timer_apply_arm(Lpc1778TimerState *s)
{
    uint32_t rate = lpc1778_timer_tc_rate(s);
    uint64_t delta = lpc1778_timer_running(s) ? lpc1778_timer_next_delta(s) : 0;

    if (delta && rate) {
        s->pending = delta;
        ptimer_set_freq(s->timer, rate);
        ptimer_set_limit(s->timer, delta, 1);
        ptimer_run(s->timer, 1);
    } else {
        s->pending = 0;
        ptimer_stop(s->timer);
    }
}

static void lpc1778_timer_rearm(Lpc1778TimerState *s)
{
    if (!s->timer) {
        return;
    }
    /* The tick callback already runs inside a ptimer transaction. */
    if (s->in_tick) {
        lpc1778_timer_apply_arm(s);
        return;
    }
    ptimer_transaction_begin(s->timer);
    lpc1778_timer_apply_arm(s);
    ptimer_transaction_commit(s->timer);
}

/* Fold the counts run so far into TC before the guest changes the setup. */
static void lpc1778_timer_sync(Lpc1778TimerState *s)
{
    s->tc += (uint32_t)lpc1778_timer_elapsed(s);
    s->pending = 0;
}

static void lpc1778_timer_tick(void *opaque)
{
    Lpc1778TimerState *s = opaque;

    if (!lpc1778_timer_running(s)) {
        return;
    }
    s->in_tick = true;
    s->tc += (uint32_t)s->pending;
    s->pending = 0;
    lpc1778_timer_check_matches(s);
    lpc1778_timer_rearm(s);
    s->in_tick = false;
}

static uint64_t lpc1778_timer_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778TimerState *s = opaque;

    (void)size;
    switch (addr) {
    case REG_IR:
        return s->ir;
    case REG_TCR:
        return s->tcr;
    case REG_TC:
        if (s->tcr & TCR_CRST) {
            return 0;
        }
        return s->tc + (uint32_t)lpc1778_timer_elapsed(s);
    case REG_PR:
        return s->pr;
    case REG_PC:
        /* Sub-count granularity is not modelled: the ptimer counts in TC. */
        return s->pc;
    case REG_MCR:
        return s->mcr;
    case REG_MR0:
    case REG_MR0 + 4:
    case REG_MR0 + 8:
    case REG_MR0 + 12:
        return s->mr[(addr - REG_MR0) / 4];
    case REG_CCR:
        return s->ccr;
    case REG_CR0:
    case REG_CR0 + 4:
    case REG_CR0 + 8:
    case REG_CR0 + 12:
        return s->cr[(addr - REG_CR0) / 4];
    case REG_EMR:
        return s->emr;
    case REG_CTCR:
        return s->ctcr;
    default:
        return 0;
    }
}

static void lpc1778_timer_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned size)
{
    Lpc1778TimerState *s = opaque;
    uint32_t old_tcr;
    int cr_i;

    (void)size;
    if (addr != REG_IR && addr != REG_CCR && addr != REG_CTCR) {
        lpc1778_timer_sync(s);
    }
    switch (addr) {
    case REG_IR:
        s->ir &= ~(uint32_t)value;
        s->ir_raised &= s->ir;
        lpc1778_timer_update_irq(s);
        break;
    case REG_TCR:
        old_tcr = s->tcr;
        s->tcr = value & 3u;
        if (s->tcr & TCR_CRST) {
            s->tc = 0;
            s->pc = 0;
        }
        lpc1778_timer_rearm(s);
        /*
         * Mech path (TIMER1/3): CRST then CEN with MCR MR3I/MR3R and MR3=0.
         * Match on the still-zero TC so IR/IRQ fire once at start, then
         * rearm for the next (now programmed) period.
         */
        if ((s->tcr & TCR_CEN) && !(s->tcr & TCR_CRST) &&
            (!(old_tcr & TCR_CEN) || (old_tcr & TCR_CRST))) {
            lpc1778_timer_check_matches(s);
            lpc1778_timer_rearm(s);
        }
        break;
    case REG_TC:
        s->tc = value;
        lpc1778_timer_rearm(s);
        break;
    case REG_PR:
        s->pr = value;
        lpc1778_timer_rearm(s);
        break;
    case REG_PC:
        s->pc = value;
        lpc1778_timer_rearm(s);
        break;
    case REG_MCR:
        s->mcr = value;
        lpc1778_timer_rearm(s);
        break;
    case REG_MR0:
    case REG_MR0 + 4:
    case REG_MR0 + 8:
    case REG_MR0 + 12:
        s->mr[(addr - REG_MR0) / 4] = value;
        lpc1778_timer_rearm(s);
        break;
    case REG_CCR:
        s->ccr = value;
        break;
    case REG_EMR:
        s->emr = value;
        break;
    case REG_CTCR:
        s->ctcr = value & 0xF;
        break;
    default:
        if (addr >= REG_CR0 && addr <= REG_CR0 + 12) {
            cr_i = (int)((addr - REG_CR0) / 4);
            s->cr[cr_i] = value;
        }
        break;
    }
}

static const MemoryRegionOps lpc1778_timer_ops = {
    .read = lpc1778_timer_read,
    .write = lpc1778_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void lpc1778_timer_reset(DeviceState *dev)
{
    Lpc1778TimerState *s = LPC1778_TIMER(dev);

    s->ir = 0;
    s->ir_raised = 0;
    s->tcr = 0;
    s->tc = 0;
    s->pr = 0;
    s->pc = 0;
    s->mcr = 0;
    s->ccr = 0;
    s->emr = 0;
    s->ctcr = 0;
    s->pending = 0;
    memset(s->mr, 0, sizeof(s->mr));
    memset(s->cr, 0, sizeof(s->cr));
    lpc1778_timer_rearm(s);
    lpc1778_timer_update_irq(s);
}

static void lpc1778_timer_init(Object *obj)
{
    Lpc1778TimerState *s = LPC1778_TIMER(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_timer_ops, s,
                          TYPE_LPC1778_TIMER, 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_timer_realize(DeviceState *dev, Error **errp)
{
    Lpc1778TimerState *s = LPC1778_TIMER(dev);

    if (!s->cclk_hz) {
        error_setg(errp, "lpc1778-timer: cclk-hz must be set by the SoC");
        return;
    }
    /* The rate follows PCLKSEL, so it is set when the counter is armed. */
    s->timer = ptimer_init(lpc1778_timer_tick, s, PTIMER_POLICY_LEGACY);
}

static const Property lpc1778_timer_properties[] = {
    DEFINE_PROP_UINT32("cclk-hz", Lpc1778TimerState, cclk_hz, 0),
    DEFINE_PROP_LINK("syscon", Lpc1778TimerState, syscon,
                     TYPE_LPC1778_SYSCON, Lpc1778SysconState *),
};

static void lpc1778_timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_timer_reset);
    device_class_set_props(dc, lpc1778_timer_properties);
    dc->realize = lpc1778_timer_realize;
}

static const TypeInfo lpc1778_timer_info = {
    .name = TYPE_LPC1778_TIMER,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778TimerState),
    .instance_init = lpc1778_timer_init,
    .class_init = lpc1778_timer_class_init,
};

static void lpc1778_timer_register_types(void)
{
    type_register_static(&lpc1778_timer_info);
}

type_init(lpc1778_timer_register_types)
