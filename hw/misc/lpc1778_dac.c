/*
 * NXP LPC1778 DAC (store-only — AOUT / speaker path).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/lpc1778_dac.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"

static uint64_t lpc1778_dac_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778DacState *s = opaque;

    switch (addr) {
    case 0x00:
        return s->dacr;
    case 0x04:
        return s->ctrl;
    case 0x08:
        return s->cntval;
    default:
        return 0;
    }
}

static void lpc1778_dac_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778DacState *s = opaque;

    switch (addr) {
    case 0x00:
        s->dacr = value;
        break;
    case 0x04:
        s->ctrl = value;
        break;
    case 0x08:
        s->cntval = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps lpc1778_dac_ops = {
    .read = lpc1778_dac_read,
    .write = lpc1778_dac_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_dac_reset(DeviceState *dev)
{
    Lpc1778DacState *s = LPC1778_DAC(dev);

    s->dacr = 0;
    s->ctrl = 0;
    s->cntval = 0;
}

static void lpc1778_dac_init(Object *obj)
{
    Lpc1778DacState *s = LPC1778_DAC(obj);

    memory_region_init_io(&s->iomem, obj, &lpc1778_dac_ops, s,
                          TYPE_LPC1778_DAC, 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_dac_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_dac_reset);
}

static const TypeInfo lpc1778_dac_info = {
    .name = TYPE_LPC1778_DAC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778DacState),
    .instance_init = lpc1778_dac_init,
    .class_init = lpc1778_dac_class_init,
};

static void lpc1778_dac_register_types(void)
{
    type_register_static(&lpc1778_dac_info);
}

type_init(lpc1778_dac_register_types)
