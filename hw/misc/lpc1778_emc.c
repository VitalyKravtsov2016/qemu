/*
 * NXP LPC1778 External Memory Controller (stub + ready STATUS)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/lpc1778_emc.h"
#include "hw/core/sysbus.h"
#include "qemu/module.h"

static uint64_t lpc1778_emc_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778EmcState *s = opaque;

    if (addr >= LPC1778_EMC_SIZE) {
        return 0;
    }
    /* STATUS: bit0 Busy = 0 (ready) */
    if (addr == 0x04) {
        return 0;
    }
    return s->regs[addr / 4];
}

static void lpc1778_emc_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778EmcState *s = opaque;

    if (addr >= LPC1778_EMC_SIZE) {
        return;
    }
    s->regs[addr / 4] = value;
}

static const MemoryRegionOps lpc1778_emc_ops = {
    .read = lpc1778_emc_read,
    .write = lpc1778_emc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_emc_reset(DeviceState *dev)
{
    Lpc1778EmcState *s = LPC1778_EMC(dev);

    memset(s->regs, 0, sizeof(s->regs));
}

static void lpc1778_emc_init(Object *obj)
{
    Lpc1778EmcState *s = LPC1778_EMC(obj);

    memory_region_init_io(&s->iomem, obj, &lpc1778_emc_ops, s,
                          TYPE_LPC1778_EMC, LPC1778_EMC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_emc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_emc_reset);
}

static const TypeInfo lpc1778_emc_info = {
    .name = TYPE_LPC1778_EMC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778EmcState),
    .instance_init = lpc1778_emc_init,
    .class_init = lpc1778_emc_class_init,
};

static void lpc1778_emc_register_types(void)
{
    type_register_static(&lpc1778_emc_info);
}

type_init(lpc1778_emc_register_types)
