/*
 * NXP LPC1778 GPIO interrupt (UM10470 ch. 9) @ 0x40028000
 *
 * Rising/falling enables on P0 and P2. Combined pending → NVIC IRQ 38.
 * Edges are sampled from the GPIO pin overlay (including QMP sensors).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/gpio/lpc1778_gpioint.h"
#include "hw/core/irq.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define REG_IO0STATR 0x080
#define REG_IO0STATF 0x084
#define REG_IO0CLR   0x088
#define REG_IO0ENR   0x08C
#define REG_IO0ENF   0x090
#define REG_IO2STATR 0x0A0
#define REG_IO2STATF 0x0A4
#define REG_IO2CLR   0x0A8
#define REG_IO2ENR   0x0AC
#define REG_IO2ENF   0x0B0
#define REG_IOSTATUS 0x180

static int bank_of(hwaddr addr)
{
    if (addr >= REG_IO2STATR && addr <= REG_IO2ENF) {
        return 1;
    }
    if (addr >= REG_IO0STATR && addr <= REG_IO0ENF) {
        return 0;
    }
    return -1;
}

static void lpc1778_gpioint_update_irq(Lpc1778GpioIntState *s)
{
    bool pending = (s->stat_r[0] | s->stat_f[0] |
                    s->stat_r[1] | s->stat_f[1]) != 0;

    qemu_set_irq(s->irq, pending);
}

static void lpc1778_gpioint_sample(void *opaque)
{
    Lpc1778GpioIntState *s = opaque;
    uint32_t cur[2];
    int i;

    if (!s->gpio) {
        return;
    }
    cur[0] = lpc1778_gpio_get_pins(s->gpio, 0);
    cur[1] = lpc1778_gpio_get_pins(s->gpio, 2);
    for (i = 0; i < 2; i++) {
        uint32_t risen = cur[i] & ~s->last[i];
        uint32_t fallen = ~cur[i] & s->last[i];

        s->stat_r[i] |= risen & s->en_r[i];
        s->stat_f[i] |= fallen & s->en_f[i];
        s->last[i] = cur[i];
    }
    lpc1778_gpioint_update_irq(s);
}

static uint64_t lpc1778_gpioint_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778GpioIntState *s = opaque;

    lpc1778_gpioint_sample(s);
    switch (addr) {
    case REG_IO0STATR:
        return s->stat_r[0];
    case REG_IO0STATF:
        return s->stat_f[0];
    case REG_IO0ENR:
        return s->en_r[0];
    case REG_IO0ENF:
        return s->en_f[0];
    case REG_IO2STATR:
        return s->stat_r[1];
    case REG_IO2STATF:
        return s->stat_f[1];
    case REG_IO2ENR:
        return s->en_r[1];
    case REG_IO2ENF:
        return s->en_f[1];
    case REG_IOSTATUS:
        return ((s->stat_r[0] | s->stat_f[0]) ? 1u : 0u) |
               ((s->stat_r[1] | s->stat_f[1]) ? 4u : 0u);
    default:
        return 0;
    }
}

static void lpc1778_gpioint_write(void *opaque, hwaddr addr,
                                  uint64_t value, unsigned size)
{
    Lpc1778GpioIntState *s = opaque;
    uint32_t v = (uint32_t)value;
    int b = bank_of(addr);

    switch (addr) {
    case REG_IO0CLR:
    case REG_IO2CLR:
        if (b >= 0) {
            s->stat_r[b] &= ~v;
            s->stat_f[b] &= ~v;
            lpc1778_gpioint_update_irq(s);
        }
        break;
    case REG_IO0ENR:
    case REG_IO2ENR:
        if (b >= 0) {
            s->en_r[b] = v;
            lpc1778_gpioint_sample(s);
        }
        break;
    case REG_IO0ENF:
    case REG_IO2ENF:
        if (b >= 0) {
            s->en_f[b] = v;
            lpc1778_gpioint_sample(s);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps lpc1778_gpioint_ops = {
    .read = lpc1778_gpioint_read,
    .write = lpc1778_gpioint_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_gpioint_reset(DeviceState *dev)
{
    Lpc1778GpioIntState *s = LPC1778_GPIOINT(dev);
    int i;

    for (i = 0; i < 2; i++) {
        s->en_r[i] = s->en_f[i] = 0;
        s->stat_r[i] = s->stat_f[i] = 0;
        s->last[i] = s->gpio ? lpc1778_gpio_get_pins(s->gpio, i ? 2 : 0) : 0;
    }
    lpc1778_gpioint_update_irq(s);
}

static void lpc1778_gpioint_realize(DeviceState *dev, Error **errp)
{
    Lpc1778GpioIntState *s = LPC1778_GPIOINT(dev);

    (void)errp;
    if (s->gpio) {
        lpc1778_gpio_add_change_handler(s->gpio, lpc1778_gpioint_sample, s);
        s->last[0] = lpc1778_gpio_get_pins(s->gpio, 0);
        s->last[1] = lpc1778_gpio_get_pins(s->gpio, 2);
    }
}

static void lpc1778_gpioint_init(Object *obj)
{
    Lpc1778GpioIntState *s = LPC1778_GPIOINT(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_gpioint_ops, s,
                          TYPE_LPC1778_GPIOINT, 0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_gpioint_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_gpioint_reset);
    dc->realize = lpc1778_gpioint_realize;
}

static const TypeInfo lpc1778_gpioint_info = {
    .name = TYPE_LPC1778_GPIOINT,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778GpioIntState),
    .instance_init = lpc1778_gpioint_init,
    .class_init = lpc1778_gpioint_class_init,
};

static void lpc1778_gpioint_register_types(void)
{
    type_register_static(&lpc1778_gpioint_info);
}

type_init(lpc1778_gpioint_register_types)
