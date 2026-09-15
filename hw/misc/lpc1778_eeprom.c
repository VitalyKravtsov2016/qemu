/*
 * NXP LPC1778 on-chip EEPROM controller (UM10470).
 * Data window 0x00200000..0x0020007F + regs @ 0x00200080.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/lpc1778_eeprom.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "system/system.h"

#define EE_REGS 0x80
#define EE_CMD  0x00
#define EE_ADDR 0x04
#define EE_WDATA 0x08
#define EE_RDATA 0x0C
#define EE_WSTATE 0x10
#define EE_CLKDIV 0x14
#define EE_PWRDWN 0x18
#define EE_INT_CLR_ENABLE 0xF58
#define EE_INT_SET_ENABLE 0xF5C
#define EE_INT_STATUS     0xF60
#define EE_INT_ENABLE     0xF64
#define EE_INT_CLR_STATUS 0xF68
#define EE_INT_SET_STATUS 0xF6C

#define EE_END_OF_RW   (1u << 26)
#define EE_END_OF_PROG (1u << 28)

static void lpc1778_eeprom_save(Lpc1778EepromState *s)
{
    g_autofree char *tmp = NULL;
    FILE *f;

    if (!s->persist_path || !s->persist_path[0]) {
        return;
    }
    tmp = g_strdup_printf("%s.tmp", s->persist_path);
    f = fopen(tmp, "wb");
    if (!f) {
        return;
    }
    if (fwrite(s->mem, 1, sizeof(s->mem), f) != sizeof(s->mem)) {
        fclose(f);
        unlink(tmp);
        return;
    }
    fclose(f);
    unlink(s->persist_path);
    if (rename(tmp, s->persist_path) != 0) {
        unlink(tmp);
    }
}

void lpc1778_eeprom_persist(Lpc1778EepromState *s)
{
    lpc1778_eeprom_save(s);
}

static void lpc1778_eeprom_load(Lpc1778EepromState *s)
{
    FILE *f;
    size_t n;

    memset(s->mem, 0xFF, sizeof(s->mem));
    if (!s->persist_path || !s->persist_path[0]) {
        return;
    }
    f = fopen(s->persist_path, "rb");
    if (!f) {
        return;
    }
    n = fread(s->mem, 1, sizeof(s->mem), f);
    fclose(f);
    info_report("on-chip EEPROM: loaded %zu bytes from %s", n, s->persist_path);
}

static void lpc1778_eeprom_exit(Notifier *n, void *opaque)
{
    Lpc1778EepromState *s = container_of(n, Lpc1778EepromState, exit);

    (void)opaque;
    lpc1778_eeprom_save(s);
}

static unsigned ee_width(Lpc1778EepromState *s)
{
    switch (s->cmd & 7u) {
    case 1:
    case 4:
        return 2;
    case 2:
    case 5:
        return 4;
    default:
        return 1;
    }
}

static void ee_complete_rw(Lpc1778EepromState *s)
{
    s->intstat |= EE_END_OF_RW;
}

static void ee_complete_prog(Lpc1778EepromState *s)
{
    s->intstat |= EE_END_OF_PROG | EE_END_OF_RW;
}

static uint64_t lpc1778_eeprom_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778EepromState *s = opaque;
    uint32_t out = 0;
    unsigned i, w;
    uint32_t a;

    if (addr < EE_REGS) {
        for (i = 0; i < size && addr + i < LPC1778_EEPROM_MEM_SIZE; i++) {
            out |= (uint32_t)s->mem[addr + i] << (8 * i);
        }
        return out;
    }

    switch ((addr - EE_REGS) & ~3u) {
    case EE_CMD:
        return s->cmd;
    case EE_ADDR:
        return s->addr;
    case EE_RDATA:
        w = ee_width(s);
        a = s->addr & (LPC1778_EEPROM_MEM_SIZE - 1);
        for (i = 0; i < w && a + i < LPC1778_EEPROM_MEM_SIZE; i++) {
            out |= (uint32_t)s->mem[a + i] << (8 * i);
        }
        s->rdata = out;
        s->addr = (s->addr + w) & 0xFFF;
        ee_complete_rw(s);
        return out;
    case EE_WSTATE:
        return s->wstate;
    case EE_CLKDIV:
        return s->clkdiv;
    case EE_PWRDWN:
        return s->pwrdwn;
    case EE_INT_STATUS:
        return s->intstat;
    case EE_INT_ENABLE:
        return s->inten;
    default:
        return 0;
    }
}

static void lpc1778_eeprom_write(void *opaque, hwaddr addr,
                                 uint64_t value, unsigned size)
{
    Lpc1778EepromState *s = opaque;
    unsigned i, w;
    uint32_t a, v = value;

    if (addr < EE_REGS) {
        for (i = 0; i < size && addr + i < LPC1778_EEPROM_MEM_SIZE; i++) {
            s->mem[addr + i] = (value >> (8 * i)) & 0xFF;
        }
        return;
    }

    switch ((addr - EE_REGS) & ~3u) {
    case EE_CMD:
        s->cmd = v;
        if ((s->cmd & 7u) == 6u) {
            ee_complete_prog(s);
            lpc1778_eeprom_save(s);
        }
        break;
    case EE_ADDR:
        s->addr = v & 0xFFF;
        break;
    case EE_WDATA:
        w = ee_width(s);
        a = s->addr & (LPC1778_EEPROM_MEM_SIZE - 1);
        for (i = 0; i < w && a + i < LPC1778_EEPROM_MEM_SIZE; i++) {
            s->mem[a + i] = (v >> (8 * i)) & 0xFF;
        }
        s->addr = (s->addr + w) & 0xFFF;
        ee_complete_rw(s);
        break;
    case EE_WSTATE:
        s->wstate = v;
        break;
    case EE_CLKDIV:
        s->clkdiv = v;
        break;
    case EE_PWRDWN:
        s->pwrdwn = v & 1u;
        break;
    case EE_INT_CLR_ENABLE:
        s->inten &= ~v;
        break;
    case EE_INT_SET_ENABLE:
        s->inten |= v;
        break;
    case EE_INT_CLR_STATUS:
        s->intstat &= ~v;
        break;
    case EE_INT_SET_STATUS:
        s->intstat |= v;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps lpc1778_eeprom_ops = {
    .read = lpc1778_eeprom_read,
    .write = lpc1778_eeprom_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_eeprom_reset(DeviceState *dev)
{
    Lpc1778EepromState *s = LPC1778_EEPROM(dev);

    /* EEPROM cells keep data across MCU reset (and across QEMU launches). */
    s->cmd = 0;
    s->addr = 0;
    s->rdata = 0xFF;
    s->wstate = 0;
    s->clkdiv = 0;
    s->pwrdwn = 0;
    s->inten = 0;
    s->intstat = 0;
}

static void lpc1778_eeprom_realize(DeviceState *dev, Error **errp)
{
    Lpc1778EepromState *s = LPC1778_EEPROM(dev);

    (void)errp;
    lpc1778_eeprom_load(s);
    s->exit.notify = lpc1778_eeprom_exit;
    qemu_add_exit_notifier(&s->exit);
}

static void lpc1778_eeprom_unrealize(DeviceState *dev)
{
    Lpc1778EepromState *s = LPC1778_EEPROM(dev);

    lpc1778_eeprom_save(s);
}

static void lpc1778_eeprom_init(Object *obj)
{
    Lpc1778EepromState *s = LPC1778_EEPROM(obj);

    memset(s->mem, 0xFF, sizeof(s->mem));
    memory_region_init_io(&s->iomem, obj, &lpc1778_eeprom_ops, s,
                          TYPE_LPC1778_EEPROM, LPC1778_EEPROM_MEM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property lpc1778_eeprom_properties[] = {
    DEFINE_PROP_STRING("persist-path", Lpc1778EepromState, persist_path),
};

static void lpc1778_eeprom_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_eeprom_reset);
    device_class_set_props(dc, lpc1778_eeprom_properties);
    dc->realize = lpc1778_eeprom_realize;
    dc->unrealize = lpc1778_eeprom_unrealize;
}

static const TypeInfo lpc1778_eeprom_info = {
    .name = TYPE_LPC1778_EEPROM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778EepromState),
    .instance_init = lpc1778_eeprom_init,
    .class_init = lpc1778_eeprom_class_init,
};

static void lpc1778_eeprom_register_types(void)
{
    type_register_static(&lpc1778_eeprom_info);
}

type_init(lpc1778_eeprom_register_types)
