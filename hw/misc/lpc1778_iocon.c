/*
 * NXP LPC1778 IOCON
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/lpc1778_iocon.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"

static uint64_t lpc1778_iocon_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778IoconState *s = opaque;

    if (addr >= LPC1778_IOCON_SIZE) {
        return 0;
    }
    return s->regs[addr / 4];
}

static void lpc1778_iocon_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned size)
{
    Lpc1778IoconState *s = opaque;

    if (addr >= LPC1778_IOCON_SIZE) {
        return;
    }
    s->regs[addr / 4] = value;
}

static const MemoryRegionOps lpc1778_iocon_ops = {
    .read = lpc1778_iocon_read,
    .write = lpc1778_iocon_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_iocon_reset(DeviceState *dev)
{
    Lpc1778IoconState *s = LPC1778_IOCON(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void lpc1778_iocon_init(Object *obj)
{
    Lpc1778IoconState *s = LPC1778_IOCON(obj);

    memory_region_init_io(&s->iomem, obj, &lpc1778_iocon_ops, s,
                          TYPE_LPC1778_IOCON, LPC1778_IOCON_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_iocon_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_iocon_reset);
}

static const TypeInfo lpc1778_iocon_info = {
    .name = TYPE_LPC1778_IOCON,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778IoconState),
    .instance_init = lpc1778_iocon_init,
    .class_init = lpc1778_iocon_class_init,
};

static void lpc1778_iocon_register_types(void)
{
    type_register_static(&lpc1778_iocon_info);
}

type_init(lpc1778_iocon_register_types)
