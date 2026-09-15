/*
 * NXP LPC1778 ADC0 — 24V_LEV / TPE / TPNE1 (SME16031).
 *
 * Firmware (ADCtask @0x31156, CMSIS-style ADDR poll @0x184a0) enables BURST
 * with SEL bits 0,2,3,6,7 and accepts a sample only when ADDR DONE (bit 31)
 * is set. Burst therefore refreshes every selected channel each tick, which
 * is what keeps DR6/DR7 live for the paper optics; reads themselves have no
 * side effect beyond clearing DONE/OVERRUN, as on silicon.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/adc/lpc1778_adc.h"
#include "hw/gpio/lpc1778_gpio.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define REG_CR    0x00
#define REG_GDR   0x04
#define REG_INTEN 0x0C
#define REG_DR0   0x10
#define REG_STAT  0x30
#define REG_TRM   0x34

#define ADC_DONE       (1u << 31)
#define ADC_OVERRUN    (1u << 30)
#define ADC_BURST      (1u << 16)
#define ADC_PDN        (1u << 21)
#define ADC_START_MASK (7u << 24)
#define ADC_START_NOW  (1u << 24)

static uint32_t adc_sample(Lpc1778AdcState *s, unsigned ch)
{
    bool paper = true;
    bool near_end = false;

    if (s->gpio) {
        paper = s->gpio->paper_present;
        near_end = s->gpio->paper_near_end;
    }
    if (ch == 0) {
        /*
         * 24V_LEV. Firmware @0x3bdb0 takes bits[11:4] of the 12-bit
         * sample at 0x1000361c and requires 0x80..0xDA (ADC 0x800..0xDAF).
         * If flag +0x2a is set, values above 0xA8C also take SYSRESETREQ.
         * 0xA00 sits in the middle of that window; 0x800 is the floor and
         * a missed sample of 0 looks like "no 24V" and resets the guest.
         */
        return 0xA00;
    }
    /*
     * TPE=AD0[6], TPNE1=AD0[7]. The optics conduct when paper is in the slot,
     * so a low sample means present: that is the level at which 10h/11h set
     * "оптический датчик ЧЛ" (bit 7) and "рулон ЧЛ" (bit 1).
     */
    if (ch == 6) {
        return paper ? 0x400 : 0xA00;
    }
    if (ch == 7) {
        return near_end ? 0x400 : 0xA00;
    }
    if (ch == 2) {
        return 0x500; /* head temp-ish */
    }
    return 0x800;
}

static void adc_irq_update(Lpc1778AdcState *s, unsigned ch)
{
    if ((s->inten & (1u << ch)) || (s->inten & (1u << 8))) {
        qemu_set_irq(s->irq, 1);
    }
}

static void adc_convert_ch(Lpc1778AdcState *s, unsigned ch)
{
    uint32_t result, word, keep;

    ch &= 7u;
    result = (adc_sample(s, ch) & 0xFFF) << 4;
    word = result | (ch << 24) | ADC_DONE;
    /*
     * OVERRUN means a result was overwritten before being read; UM10470
     * defines it for burst mode only. Both flags stay until the register
     * is read.
     */
    keep = (s->cr & ADC_BURST) ? (s->dr[ch] & ADC_DONE) : 0;
    s->gdr = word | ((s->gdr & ADC_DONE) && (s->cr & ADC_BURST) ?
                     ADC_OVERRUN : 0);
    s->dr[ch] = word | (keep ? ADC_OVERRUN : 0);
    adc_irq_update(s, ch);
}

static unsigned adc_lowest_sel(uint32_t sel)
{
    int i;

    for (i = 0; i < 8; i++) {
        if (sel & (1u << i)) {
            return (unsigned)i;
        }
    }
    return 0;
}

/* Burst is enabled only with START = 000 (UM10470 32.5.1 remark). */
static bool adc_burst_active(const Lpc1778AdcState *s)
{
    return (s->cr & ADC_BURST) && (s->cr & ADC_PDN) && (s->cr & 0xFF) &&
           !(s->cr & ADC_START_MASK);
}

/* Software-controlled single conversion of the selected channel. */
static void adc_convert_sel(Lpc1778AdcState *s)
{
    uint32_t sel = s->cr & 0xFF;

    if (!(s->cr & ADC_PDN) || !sel) {
        return;
    }
    adc_convert_ch(s, adc_lowest_sel(sel));
}

static void adc_burst_rearm(Lpc1778AdcState *s)
{
    if (!s->burst) {
        return;
    }
    ptimer_transaction_begin(s->burst);
    if (adc_burst_active(s)) {
        ptimer_set_limit(s->burst, 1, 1);
        ptimer_run(s->burst, 0);
    } else {
        ptimer_stop(s->burst);
    }
    ptimer_transaction_commit(s->burst);
}

/*
 * UM10470: burst walks SEL from low channel to high, repeatedly. One sweep
 * per tick keeps every enabled channel fresh for a polling guest without a
 * callback per conversion.
 */
static void adc_burst_sweep(Lpc1778AdcState *s)
{
    uint32_t sel = s->cr & 0xFF;
    unsigned start = s->burst_ch;
    unsigned i, ch;

    for (i = 1; i <= 8; i++) {
        ch = (start + i) & 7u;
        if (sel & (1u << ch)) {
            adc_convert_ch(s, ch);
            s->burst_ch = ch;
        }
    }
}

static void adc_burst_tick(void *opaque)
{
    Lpc1778AdcState *s = opaque;

    if (!adc_burst_active(s)) {
        return;
    }
    adc_burst_sweep(s);
}

static uint32_t adc_read_dr(Lpc1778AdcState *s, unsigned ch)
{
    uint32_t word;

    ch &= 7u;
    word = s->dr[ch];
    /* DONE and OVERRUN are cleared by reading the register. */
    s->dr[ch] &= ~(ADC_DONE | ADC_OVERRUN);
    qemu_set_irq(s->irq, 0);
    return word;
}

static uint64_t lpc1778_adc_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778AdcState *s = opaque;
    int i;

    switch (addr) {
    case REG_CR:
        return s->cr;
    case REG_GDR: {
        uint32_t gdr = s->gdr;

        s->gdr &= ~(ADC_DONE | ADC_OVERRUN);
        qemu_set_irq(s->irq, 0);
        return gdr;
    }
    case REG_INTEN:
        return s->inten;
    case REG_TRM:
        return s->trm;
    case REG_STAT: {
        /* DONE0..7 in [7:0], OVERRUN0..7 in [15:8], ADINT in bit 16. */
        uint32_t st = 0;
        for (i = 0; i < 8; i++) {
            if (s->dr[i] & ADC_DONE) {
                st |= 1u << i;
            }
            if (s->dr[i] & ADC_OVERRUN) {
                st |= 1u << (8 + i);
            }
        }
        if (s->gdr & ADC_DONE) {
            st |= 1u << 16;
        }
        return st;
    }
    default:
        if (addr >= REG_DR0 && addr < REG_DR0 + 32) {
            return adc_read_dr(s, (addr - REG_DR0) / 4);
        }
        return 0;
    }
}

static void lpc1778_adc_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778AdcState *s = opaque;

    switch (addr) {
    case REG_CR: {
        bool was_burst = adc_burst_active(s);

        s->cr = value;
        /*
         * Only START = 001 converts now. 010..111 wait for an edge on
         * P2.10/P1.27 or a timer match, which this model never generates.
         */
        if ((value & ADC_START_MASK) == ADC_START_NOW) {
            adc_convert_sel(s);
        }
        if (adc_burst_active(s) && !was_burst) {
            /* Burst starts converting on the write, not a tick later. */
            adc_burst_sweep(s);
        }
        adc_burst_rearm(s);
        break;
    }
    case REG_INTEN:
        s->inten = value;
        break;
    case REG_TRM:
        s->trm = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps lpc1778_adc_ops = {
    .read = lpc1778_adc_read,
    .write = lpc1778_adc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_adc_reset(DeviceState *dev)
{
    Lpc1778AdcState *s = LPC1778_ADC(dev);

    s->cr = 1;        /* SEL reset value 0x01 */
    s->gdr = 0;
    s->inten = 0x100; /* ADGINTEN set after reset */
    s->trm = 0;
    s->burst_ch = 0;
    memset(s->dr, 0, sizeof(s->dr));
    adc_burst_rearm(s);
}

static void lpc1778_adc_init(Object *obj)
{
    Lpc1778AdcState *s = LPC1778_ADC(obj);

    memory_region_init_io(&s->iomem, obj, &lpc1778_adc_ops, s,
                          TYPE_LPC1778_ADC, 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void lpc1778_adc_realize(DeviceState *dev, Error **errp)
{
    Lpc1778AdcState *s = LPC1778_ADC(dev);

    (void)errp;
    s->burst = ptimer_init(adc_burst_tick, s, PTIMER_POLICY_LEGACY);
    ptimer_transaction_begin(s->burst);
    /*
     * One SEL sweep per tick. Hardware repeats a sweep every ~20 us at
     * 400 kS/s; 10 kHz keeps DONE set for any realistic poll without
     * spending a host callback per conversion.
     */
    ptimer_set_freq(s->burst, 10000);
    ptimer_transaction_commit(s->burst);
}

static void lpc1778_adc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_adc_reset);
    dc->realize = lpc1778_adc_realize;
}

static const TypeInfo lpc1778_adc_info = {
    .name = TYPE_LPC1778_ADC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778AdcState),
    .instance_init = lpc1778_adc_init,
    .class_init = lpc1778_adc_class_init,
};

static void lpc1778_adc_register_types(void)
{
    type_register_static(&lpc1778_adc_info);
}

type_init(lpc1778_adc_register_types)
