/*
 * NXP LPC1778 GPDMA (PL080-like) — runs channel transfers immediately.
 * Used by firmware for SSP0 EEPROM (W25Q) M2P/P2M paths.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/lpc1778_gpdma.h"
#include "hw/core/irq.h"
#include "system/dma.h"
#include "qemu/module.h"

#define REG_INTSTAT      0x000
#define REG_INTTCSTAT    0x004
#define REG_INTTCCLEAR   0x008
#define REG_INTERRSTAT   0x00C
#define REG_INTERRCLR    0x010
#define REG_RAWINTTCSTAT 0x014
#define REG_RAWINTERRSTAT 0x018
#define REG_ENBLDCHNS    0x01C
#define REG_SOFTBREQ     0x020
#define REG_SOFTSREQ     0x024
#define REG_SOFTLBREQ    0x028
#define REG_SOFTLSREQ    0x02C
#define REG_CONFIG       0x030
#define REG_SYNC         0x034
#define REG_CH0_SRC      0x100

static void lpc1778_gpdma_update_irq(Lpc1778GpdmaState *s)
{
    qemu_set_irq(s->irq, (s->int_tc | s->int_err) != 0);
}

static void lpc1778_gpdma_run_channel(Lpc1778GpdmaState *s, unsigned ch)
{
    struct Lpc1778GpdmaChan *c;
    uint32_t ctrl, xfer, src, dst;
    unsigned sw, dw, si, di, src_bytes, dst_bytes;
    uint32_t i;
    MemTxResult res;

    if (ch >= 8) {
        return;
    }
    c = &s->ch[ch];
    if ((s->config & 1u) == 0 || (c->config & 1u) == 0) {
        return;
    }

    ctrl = c->control;
    xfer = ctrl & 0xFFFu;
    if (xfer == 0) {
        xfer = 4096;
    }
    sw = (ctrl >> 18) & 7u;
    dw = (ctrl >> 21) & 7u;
    si = (ctrl >> 26) & 1u;
    di = (ctrl >> 27) & 1u;
    src_bytes = 1u << (sw > 2 ? 2 : sw);
    dst_bytes = 1u << (dw > 2 ? 2 : dw);

    src = c->src;
    dst = c->dst;
    for (i = 0; i < xfer; i++) {
        uint8_t buf[4] = {0};

        res = address_space_read(&address_space_memory, src,
                                 MEMTXATTRS_UNSPECIFIED, buf, src_bytes);
        if (res != MEMTX_OK) {
            memset(buf, 0, sizeof(buf));
        }
        res = address_space_write(&address_space_memory, dst,
                                  MEMTXATTRS_UNSPECIFIED, buf, dst_bytes);
        (void)res;

        if (si) {
            src += src_bytes;
        }
        if (di) {
            dst += dst_bytes;
        }
    }

    c->src = src;
    c->dst = dst;
    c->config &= ~1u;
    s->raw_tc |= 1u << ch;
    if (c->config & (1u << 15)) {
        s->int_tc |= 1u << ch;
    }
    lpc1778_gpdma_update_irq(s);
}

static uint64_t lpc1778_gpdma_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778GpdmaState *s = opaque;
    unsigned ch, cof, i;
    uint32_t m;

    if (addr >= REG_CH0_SRC && addr < REG_CH0_SRC + 8 * 0x20) {
        ch = (addr - REG_CH0_SRC) / 0x20;
        cof = (addr - REG_CH0_SRC) % 0x20;
        switch (cof) {
        case 0x00:
            return s->ch[ch].src;
        case 0x04:
            return s->ch[ch].dst;
        case 0x08:
            return s->ch[ch].lli;
        case 0x0C:
            return s->ch[ch].control;
        case 0x10:
            return s->ch[ch].config;
        default:
            return 0;
        }
    }

    switch (addr) {
    case REG_INTSTAT:
        return (s->int_tc | s->int_err) & 0xFF;
    case REG_INTTCSTAT:
        return s->int_tc;
    case REG_INTERRSTAT:
        return s->int_err;
    case REG_RAWINTTCSTAT:
        return s->raw_tc;
    case REG_RAWINTERRSTAT:
        return s->raw_err;
    case REG_ENBLDCHNS:
        m = 0;
        for (i = 0; i < 8; i++) {
            if (s->ch[i].config & 1u) {
                m |= 1u << i;
            }
        }
        return m;
    case REG_SOFTBREQ:
        return s->soft_breq;
    case REG_SOFTSREQ:
        return s->soft_sreq;
    case REG_SOFTLBREQ:
        return s->soft_lbreq;
    case REG_SOFTLSREQ:
        return s->soft_lsreq;
    case REG_CONFIG:
        return s->config;
    case REG_SYNC:
        return s->sync;
    default:
        return 0;
    }
}

static void lpc1778_gpdma_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned size)
{
    Lpc1778GpdmaState *s = opaque;
    unsigned ch, cof;

    if (addr >= REG_CH0_SRC && addr < REG_CH0_SRC + 8 * 0x20) {
        ch = (addr - REG_CH0_SRC) / 0x20;
        cof = (addr - REG_CH0_SRC) % 0x20;
        switch (cof) {
        case 0x00:
            s->ch[ch].src = value;
            break;
        case 0x04:
            s->ch[ch].dst = value;
            break;
        case 0x08:
            s->ch[ch].lli = value;
            break;
        case 0x0C:
            s->ch[ch].control = value;
            break;
        case 0x10:
            s->ch[ch].config = value;
            if (value & 1u) {
                lpc1778_gpdma_run_channel(s, ch);
            }
            break;
        default:
            break;
        }
        return;
    }

    switch (addr) {
    case REG_INTTCCLEAR:
        s->int_tc &= ~value;
        s->raw_tc &= ~value;
        lpc1778_gpdma_update_irq(s);
        break;
    case REG_INTERRCLR:
        s->int_err &= ~value;
        s->raw_err &= ~value;
        lpc1778_gpdma_update_irq(s);
        break;
    case REG_SOFTBREQ:
        s->soft_breq = value;
        break;
    case REG_SOFTSREQ:
        s->soft_sreq = value;
        break;
    case REG_SOFTLBREQ:
        s->soft_lbreq = value;
        break;
    case REG_SOFTLSREQ:
        s->soft_lsreq = value;
        break;
    case REG_CONFIG:
        s->config = value & 7u;
        break;
    case REG_SYNC:
        s->sync = value;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps lpc1778_gpdma_ops = {
    .read = lpc1778_gpdma_read,
    .write = lpc1778_gpdma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_gpdma_reset(DeviceState *dev)
{
    Lpc1778GpdmaState *s = LPC1778_GPDMA(dev);

    s->config = 0;
    s->sync = 0;
    s->int_tc = 0;
    s->int_err = 0;
    s->raw_tc = 0;
    s->raw_err = 0;
    s->soft_breq = s->soft_sreq = s->soft_lbreq = s->soft_lsreq = 0;
    memset(s->ch, 0, sizeof(s->ch));
    lpc1778_gpdma_update_irq(s);
}

static void lpc1778_gpdma_init(Object *obj)
{
    Lpc1778GpdmaState *s = LPC1778_GPDMA(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_gpdma_ops, s,
                          TYPE_LPC1778_GPDMA, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_gpdma_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_gpdma_reset);
}

static const TypeInfo lpc1778_gpdma_info = {
    .name = TYPE_LPC1778_GPDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778GpdmaState),
    .instance_init = lpc1778_gpdma_init,
    .class_init = lpc1778_gpdma_class_init,
};

static void lpc1778_gpdma_register_types(void)
{
    type_register_static(&lpc1778_gpdma_info);
}

type_init(lpc1778_gpdma_register_types)
