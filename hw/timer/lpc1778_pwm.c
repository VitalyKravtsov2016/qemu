/*
 * NXP LPC1778 PWM1 (UM10470 ch. 26) @ 0x40018000.
 *
 * Firmware inits PWM1 (PR=5, MR0/MR1=1000, match-int bits in MCR) and the
 * mechanism/sensor path waits on IR. PWM1_IRQn = 9 is the app default
 * handler (infinite loop), so PWM1 is not wired to NVIC. PWM0 (IRQ 39)
 * uses this same model and is connected; the line stays low until enabled.
 *
 * TC advances at PCLK/(PR+1). Ticking the ptimer once per count at that
 * rate is far too costly, so the counter is only advanced to the next
 * armed match; PC is therefore not a live prescale readback.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/timer/lpc1778_pwm.h"
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
#define REG_MR1  0x1C
#define REG_MR2  0x20
#define REG_MR3  0x24
#define REG_MR4  0x40
#define REG_MR5  0x44
#define REG_MR6  0x48
#define REG_PCR  0x4C
#define REG_LER  0x50
#define REG_CTCR 0x70

#define TCR_CEN   (1u << 0)
#define TCR_CRST  (1u << 1)
#define TCR_PWMEN (1u << 3)

static int pwm_mr_index(hwaddr addr)
{
    switch (addr) {
    case REG_MR0:
        return 0;
    case REG_MR1:
        return 1;
    case REG_MR2:
        return 2;
    case REG_MR3:
        return 3;
    case REG_MR4:
        return 4;
    case REG_MR5:
        return 5;
    case REG_MR6:
        return 6;
    default:
        return -1;
    }
}

static uint32_t pwm_ir_bit(int ch)
{
    if (ch <= 3) {
        return 1u << ch;
    }
    /* MR4..MR6 → IR bits 8..10 */
    return 1u << (ch + 4);
}

/* UM10470 26.6.11: shadow MRn become effective at the MR0 match only. */
static void lpc1778_pwm_apply_ler(Lpc1778PwmState *s)
{
    int i;

    for (i = 0; i < 7; i++) {
        if (s->ler & (1u << i)) {
            s->mr[i] = s->shadow[i];
        }
    }
    /* "Once the transfer has taken place, all bits of the LER are cleared." */
    s->ler = 0;
}

/* A match register only matters if MCR arms an action or PCR enables output. */
static bool pwm_match_armed(const Lpc1778PwmState *s, int i)
{
    if (s->mcr & (7u << ((uint32_t)i * 3u))) {
        return true;
    }
    return i >= 1 && i <= 6 && (s->pcr & (1u << (8 + i)));
}

static uint32_t pwm_tc_rate(const Lpc1778PwmState *s)
{
    uint32_t pclk = lpc1778_syscon_pclk_hz(s->syscon, s->cclk_hz);

    return pclk / (s->pr + 1u);
}

/* TC counts until the nearest armed match; 0 means nothing is scheduled. */
static uint32_t pwm_next_delta(const Lpc1778PwmState *s)
{
    uint32_t best = 0;
    int i;

    for (i = 0; i < 7; i++) {
        uint32_t d;

        if (s->mr[i] <= s->tc || !pwm_match_armed(s, i)) {
            continue;
        }
        d = s->mr[i] - s->tc;
        if (!best || d < best) {
            best = d;
        }
    }
    return best;
}

static void pwm_apply_arm(Lpc1778PwmState *s)
{
    uint32_t delta = ((s->tcr & TCR_CEN) && !(s->tcr & TCR_CRST)) ?
                     pwm_next_delta(s) : 0;
    uint32_t rate = pwm_tc_rate(s);

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

static void lpc1778_pwm_rearm(Lpc1778PwmState *s)
{
    if (!s->timer) {
        return;
    }
    /* The tick callback already runs inside a ptimer transaction. */
    if (s->in_tick) {
        pwm_apply_arm(s);
        return;
    }
    ptimer_transaction_begin(s->timer);
    pwm_apply_arm(s);
    ptimer_transaction_commit(s->timer);
}

static void lpc1778_pwm_tick(void *opaque)
{
    Lpc1778PwmState *s = opaque;
    bool reset_cycle = false;
    bool reset_by_mr0 = false;
    int i;

    if (!(s->tcr & TCR_CEN) || (s->tcr & TCR_CRST)) {
        return;
    }

    s->in_tick = true;
    s->tc += s->pending;
    s->pending = 0;

    for (i = 0; i < 7; i++) {
        uint32_t mcr_shift = (uint32_t)i * 3u;

        if (s->tc != s->mr[i]) {
            continue;
        }
        if (s->mcr & (1u << mcr_shift)) {
            s->ir |= pwm_ir_bit(i);
        }
        /* PWM1[1..6] = MR1..MR6. PCR PWMENAn is bit 8+n. Match = falling. */
        if (s->match_cb && i >= 1 && i <= 6 && (s->pcr & (1u << (8 + i)))) {
            s->match_cb(s->match_opaque, i);
        }
        if (s->mcr & (1u << (mcr_shift + 1))) {
            reset_cycle = true;
            reset_by_mr0 |= (i == 0);
        }
        if (s->mcr & (1u << (mcr_shift + 2))) {
            s->tcr &= ~TCR_CEN;
        }
    }

    if (reset_cycle) {
        s->tc = 0;
        s->pc = 0;
        if (s->tcr & TCR_PWMEN) {
            lpc1778_pwm_apply_ler(s);
        }
        /*
         * Single-edge outputs are Set by Match 0 (UM10470 Table 551), so the
         * rising edge belongs to the MR0 match, not to any other reset match.
         */
        if (s->match_cb && reset_by_mr0) {
            s->match_cb(s->match_opaque, 0);
        }
    }
    qemu_set_irq(s->irq, s->ir != 0);
    lpc1778_pwm_rearm(s);
    s->in_tick = false;
}

static uint64_t lpc1778_pwm_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778PwmState *s = opaque;
    int idx;

    switch (addr) {
    case REG_IR:
        return s->ir;
    case REG_TCR:
        return s->tcr;
    case REG_TC:
        if (s->tcr & TCR_CRST) {
            return 0;
        }
        return s->tc;
    case REG_PR:
        return s->pr;
    case REG_PC:
        return s->pc;
    case REG_MCR:
        return s->mcr;
    case REG_PCR:
        return s->pcr;
    case REG_LER:
        return s->ler;
    case REG_CTCR:
        return s->ctcr;
    default:
        idx = pwm_mr_index(addr);
        if (idx >= 0) {
            return s->mr[idx];
        }
        return 0;
    }
}

static void lpc1778_pwm_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778PwmState *s = opaque;
    int idx;

    switch (addr) {
    case REG_IR:
        s->ir &= ~(uint32_t)value;
        qemu_set_irq(s->irq, s->ir != 0);
        break;
    case REG_TCR: {
        uint32_t old = s->tcr;

        s->tcr = value & (TCR_CEN | TCR_CRST | TCR_PWMEN);
        if (s->tcr & TCR_CRST) {
            s->tc = 0;
            s->pc = 0;
        } else if ((s->tcr ^ old) & TCR_PWMEN) {
            /* UM10470 Table 555: PWM mode resets TC to 1, timer mode to 0. */
            s->tc = (s->tcr & TCR_PWMEN) ? 1 : 0;
            s->pc = 0;
        }
        lpc1778_pwm_rearm(s);
        break;
    }
    case REG_TC:
        s->tc = value;
        lpc1778_pwm_rearm(s);
        break;
    case REG_PR:
        s->pr = value;
        lpc1778_pwm_rearm(s);
        break;
    case REG_PC:
        s->pc = value;
        lpc1778_pwm_rearm(s);
        break;
    case REG_MCR:
        s->mcr = value;
        lpc1778_pwm_rearm(s);
        break;
    case REG_PCR:
        s->pcr = value;
        lpc1778_pwm_rearm(s);
        break;
    case REG_LER:
        /* Arms the transfer only; it happens at the next MR0 match. */
        s->ler = value & 0x7F;
        break;
    case REG_CTCR:
        s->ctcr = value;
        break;
    default:
        idx = pwm_mr_index(addr);
        if (idx >= 0) {
            s->shadow[idx] = value;
            /*
             * In PWM mode a write lands in the shadow register and needs both
             * the LER bit and an MR0 match; in timer mode it is immediate.
             */
            if (!(s->tcr & TCR_PWMEN)) {
                s->mr[idx] = value;
            }
            lpc1778_pwm_rearm(s);
        }
        break;
    }
}

static const MemoryRegionOps lpc1778_pwm_ops = {
    .read = lpc1778_pwm_read,
    .write = lpc1778_pwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_pwm_reset(DeviceState *dev)
{
    Lpc1778PwmState *s = LPC1778_PWM(dev);

    s->ir = 0;
    s->tcr = 0;
    s->tc = 0;
    s->pr = 0;
    s->pc = 0;
    s->mcr = 0;
    s->pcr = 0;
    s->ler = 0;
    s->ctcr = 0;
    s->pending = 0;
    memset(s->mr, 0, sizeof(s->mr));
    memset(s->shadow, 0, sizeof(s->shadow));
    lpc1778_pwm_rearm(s);
    qemu_set_irq(s->irq, 0);
}

static void lpc1778_pwm_init(Object *obj)
{
    Lpc1778PwmState *s = LPC1778_PWM(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_pwm_ops, s,
                          TYPE_LPC1778_PWM, 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_pwm_realize(DeviceState *dev, Error **errp)
{
    Lpc1778PwmState *s = LPC1778_PWM(dev);

    if (!s->cclk_hz) {
        error_setg(errp, "lpc1778-pwm: cclk-hz must be set by the SoC");
        return;
    }
    s->timer = ptimer_init(lpc1778_pwm_tick, s, PTIMER_POLICY_LEGACY);
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, s->cclk_hz);
    ptimer_transaction_commit(s->timer);
}

static const Property lpc1778_pwm_properties[] = {
    DEFINE_PROP_UINT32("cclk-hz", Lpc1778PwmState, cclk_hz, 0),
    DEFINE_PROP_LINK("syscon", Lpc1778PwmState, syscon,
                     TYPE_LPC1778_SYSCON, Lpc1778SysconState *),
};

static void lpc1778_pwm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_pwm_reset);
    device_class_set_props(dc, lpc1778_pwm_properties);
    dc->realize = lpc1778_pwm_realize;
}

static const TypeInfo lpc1778_pwm_info = {
    .name = TYPE_LPC1778_PWM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778PwmState),
    .instance_init = lpc1778_pwm_init,
    .class_init = lpc1778_pwm_class_init,
};

static void lpc1778_pwm_register_types(void)
{
    type_register_static(&lpc1778_pwm_info);
}

type_init(lpc1778_pwm_register_types)
