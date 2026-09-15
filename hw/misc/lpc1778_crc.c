/*
 * NXP LPC1778 CRC engine (UM10470)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/lpc1778_crc.h"
#include "hw/core/sysbus.h"
#include "qemu/log.h"
#include "qemu/module.h"

static uint32_t bitrev(uint32_t x, int width)
{
    uint32_t r = 0;
    int i;

    for (i = 0; i < width; i++) {
        if (x & (1u << i)) {
            r |= 1u << (width - 1 - i);
        }
    }
    return r;
}

static int crc_width(uint32_t mode)
{
    return ((mode & 3) < 2) ? 16 : 32;
}

static uint32_t crc_poly(uint32_t mode)
{
    switch (mode & 3) {
    case 0:
        return 0x1021;
    case 1:
        return 0x8005;
    default:
        return 0x04C11DB7;
    }
}

static void crc_write_data(Lpc1778CrcState *s, uint32_t value, int bits)
{
    uint32_t mask, crc, poly, bit, msb;
    int i, width;

    value &= bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    if (s->mode & 4) {
        value = bitrev(value, bits);
    }
    if (s->mode & 8) {
        value ^= bits == 32 ? 0xFFFFFFFFu : ((1u << bits) - 1);
    }

    width = crc_width(s->mode);
    poly = crc_poly(s->mode);
    mask = width == 32 ? 0xFFFFFFFFu : 0xFFFF;
    crc = s->crc & mask;

    for (i = bits - 1; i >= 0; i--) {
        bit = (value >> i) & 1;
        msb = (crc >> (width - 1)) & 1;
        crc = (crc << 1) & mask;
        if (msb ^ bit) {
            crc ^= poly;
        }
    }
    s->crc = crc;
}

static uint32_t crc_result(Lpc1778CrcState *s)
{
    int width = crc_width(s->mode);
    uint32_t mask = width == 32 ? 0xFFFFFFFFu : 0xFFFF;
    uint32_t out = s->crc & mask;

    if (s->mode & 0x10) {
        out = bitrev(out, width);
    }
    if (s->mode & 0x20) {
        out ^= mask;
    }
    return out;
}

static uint64_t lpc1778_crc_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778CrcState *s = opaque;

    switch (addr) {
    case 0x00:
        return s->mode;
    case 0x04:
        return s->seed;
    case 0x08:
        return crc_result(s);
    default:
        qemu_log_mask(LOG_UNIMP, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
}

static void lpc1778_crc_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778CrcState *s = opaque;
    int bits;

    switch (addr) {
    case 0x00:
        s->mode = value & 0x3F;
        break;
    case 0x04:
        s->seed = value;
        s->crc = value;
        break;
    case 0x08:
        bits = size == 1 ? 8 : size == 2 ? 16 : 32;
        crc_write_data(s, value, bits);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps lpc1778_crc_ops = {
    .read = lpc1778_crc_read,
    .write = lpc1778_crc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void lpc1778_crc_reset(DeviceState *dev)
{
    Lpc1778CrcState *s = LPC1778_CRC(dev);

    s->mode = 0;
    s->seed = 0xFFFF;
    s->crc = 0xFFFF;
}

static void lpc1778_crc_init(Object *obj)
{
    Lpc1778CrcState *s = LPC1778_CRC(obj);

    memory_region_init_io(&s->iomem, obj, &lpc1778_crc_ops, s,
                          TYPE_LPC1778_CRC, 0x10);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_crc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_crc_reset);
}

static const TypeInfo lpc1778_crc_info = {
    .name = TYPE_LPC1778_CRC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778CrcState),
    .instance_init = lpc1778_crc_init,
    .class_init = lpc1778_crc_class_init,
};

static void lpc1778_crc_register_types(void)
{
    type_register_static(&lpc1778_crc_info);
}

type_init(lpc1778_crc_register_types)
