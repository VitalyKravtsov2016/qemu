/*
 * NXP LPC1778 SYSCON (clock / PLL / power)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/lpc1778_syscon.h"
#include "hw/core/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define RSID      0x180
#define SCS       0x1A0
#define PCLKSEL   0x1A8
#define PLL0CON   0x080
#define PLL0CFG   0x084
#define PLL0STAT  0x088
#define PLL0FEED  0x08C
#define PLL1CON   0x0A0
#define PLL1CFG   0x0A4
#define PLL1STAT  0x0A8
#define PLL1FEED  0x0AC

static uint32_t *regp(Lpc1778SysconState *s, hwaddr addr)
{
    return &s->regs[addr / 4];
}

static uint64_t lpc1778_syscon_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778SysconState *s = opaque;
    uint32_t val;

    if (addr + size > LPC1778_SYSCON_SIZE) {
        return 0;
    }

    switch (addr) {
    case RSID:
        return s->rsid;
    case SCS:
        val = *regp(s, SCS);
        if (val & (1u << 5)) {
            val |= 1u << 6; /* OSCSTAT */
            *regp(s, SCS) = val;
        }
        return val;
    case PLL0STAT: {
        uint32_t cfg = *regp(s, PLL0CFG);
        uint32_t con = *regp(s, PLL0CON);
        val = (cfg & 0x7FFF) | ((con & 3) << 8);
        if (con & 1) {
            val |= 1u << 10; /* PLOCK */
        }
        *regp(s, PLL0STAT) = val;
        return val;
    }
    case PLL1STAT: {
        uint32_t cfg = *regp(s, PLL1CFG);
        uint32_t con = *regp(s, PLL1CON);
        val = (cfg & 0x7FFF) | ((con & 3) << 8);
        if (con & 1) {
            val |= 1u << 10;
        }
        *regp(s, PLL1STAT) = val;
        return val;
    }
    default:
        return *regp(s, addr);
    }
}

static void lpc1778_syscon_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned size)
{
    Lpc1778SysconState *s = opaque;

    if (addr + size > LPC1778_SYSCON_SIZE) {
        return;
    }

    switch (addr) {
    case RSID:
        /* Write-1-to-clear. Bits are set only by reset sources. */
        s->rsid &= ~(uint32_t)value;
        break;
    case PLL0FEED:
        if (value == 0xAA) {
            s->pll0_feed = 1;
        } else if (value == 0x55 && s->pll0_feed) {
            s->pll0_feed = 0;
        } else {
            s->pll0_feed = 0;
        }
        *regp(s, addr) = value;
        break;
    case PLL1FEED:
        if (value == 0xAA) {
            s->pll1_feed = 1;
        } else if (value == 0x55 && s->pll1_feed) {
            s->pll1_feed = 0;
        } else {
            s->pll1_feed = 0;
        }
        *regp(s, addr) = value;
        break;
    default:
        *regp(s, addr) = value;
        break;
    }
}

static const MemoryRegionOps lpc1778_syscon_ops = {
    .read = lpc1778_syscon_read,
    .write = lpc1778_syscon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void lpc1778_syscon_note_reset(Lpc1778SysconState *s, uint32_t rsid_bits)
{
    if (s) {
        s->pending_rsid |= rsid_bits;
    }
}

/*
 * PCLKSEL[4:0] divides CCLK down to the APB clock. A divider of 0 gates the
 * peripheral clock; the reset value is 1, so PCLK follows CCLK until the
 * guest programs it.
 */
uint32_t lpc1778_syscon_pclk_hz(Lpc1778SysconState *s, uint32_t cclk_hz)
{
    uint32_t div;

    if (!s) {
        return cclk_hz;
    }
    div = *regp(s, PCLKSEL) & 0x1Fu;
    return div ? cclk_hz / div : 0;
}

static void lpc1778_syscon_reset(DeviceState *dev)
{
    Lpc1778SysconState *s = LPC1778_SYSCON(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->pll0_feed = 0;
    s->pll1_feed = 0;
    *regp(s, 0x000) = 0x0000; /* FLASHCFG */
    *regp(s, PCLKSEL) = 0x01; /* PCLKSEL reset value: PCLK = CCLK */
    *regp(s, 0x104) = 0x01;   /* CCLKSEL: CPU clock = sysclk */
    /*
     * RSID survives as the cause of *this* reset. Cold start / POR leaves
     * only POR. Watchdog and SYSRESETREQ set pending_rsid first.
     */
    if (s->pending_rsid) {
        s->rsid = s->pending_rsid;
        s->pending_rsid = 0;
    } else {
        s->rsid = LPC1778_RSID_POR;
    }
}

static void lpc1778_syscon_init(Object *obj)
{
    Lpc1778SysconState *s = LPC1778_SYSCON(obj);

    memory_region_init_io(&s->iomem, obj, &lpc1778_syscon_ops, s,
                          TYPE_LPC1778_SYSCON, LPC1778_SYSCON_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_syscon_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_syscon_reset);
}

static const TypeInfo lpc1778_syscon_info = {
    .name = TYPE_LPC1778_SYSCON,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778SysconState),
    .instance_init = lpc1778_syscon_init,
    .class_init = lpc1778_syscon_class_init,
};

static void lpc1778_syscon_register_types(void)
{
    type_register_static(&lpc1778_syscon_info);
}

type_init(lpc1778_syscon_register_types)
