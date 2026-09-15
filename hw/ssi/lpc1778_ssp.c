/*
 * NXP LPC1778 SSP0/1/2 (UM10470 ch. 21).
 *
 * The register map is the PrimeCell PL022 one the SSP is built from, but the
 * transmit path must keep running when nobody reads the receive FIFO: the
 * thermal head on SSP0 is write-only, so on silicon the RX FIFO overruns
 * (RORRIS) while the shift register keeps clocking dots out. QEMU's generic
 * PL022 model deliberately stalls TX in that case, which cuts the dot data
 * off after eight words.
 *
 * The transfer itself is instantaneous: a word written to DR is clocked out
 * right away, so TFE/TNF are always set and BSY is never seen by the guest.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/ssi/lpc1778_ssp.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define REG_CR0   0x00
#define REG_CR1   0x04
#define REG_DR    0x08
#define REG_SR    0x0C
#define REG_CPSR  0x10
#define REG_IMSC  0x14
#define REG_RIS   0x18
#define REG_MIS   0x1C
#define REG_ICR   0x20
#define REG_DMACR 0x24

#define CR1_LBM   (1u << 0)
#define CR1_SSE   (1u << 1)
#define CR1_MS    (1u << 2)

#define SR_TFE    (1u << 0)
#define SR_TNF    (1u << 1)
#define SR_RNE    (1u << 2)
#define SR_RFF    (1u << 3)
#define SR_BSY    (1u << 4)

#define RIS_ROR   (1u << 0)
#define RIS_RT    (1u << 1)
#define RIS_RX    (1u << 2)
#define RIS_TX    (1u << 3)

unsigned lpc1778_ssp_word_bits(const Lpc1778SspState *s)
{
    return (s->cr0 & 0xFu) + 1u;
}

static uint32_t lpc1778_ssp_mask(const Lpc1778SspState *s)
{
    return (1u << lpc1778_ssp_word_bits(s)) - 1u;
}

static void lpc1778_ssp_update_irq(Lpc1778SspState *s)
{
    /* TX FIFO is always empty here, so TXRIS is always pending. */
    s->ris |= RIS_TX;
    if (s->rx_len >= LPC1778_SSP_FIFO / 2) {
        s->ris |= RIS_RX;
    } else {
        s->ris &= ~RIS_RX;
    }
    qemu_set_irq(s->irq, (s->ris & s->imsc) != 0);
}

static uint32_t lpc1778_ssp_status(const Lpc1778SspState *s)
{
    uint32_t sr = SR_TFE | SR_TNF;

    if (s->rx_len) {
        sr |= SR_RNE;
    }
    if (s->rx_len >= LPC1778_SSP_FIFO) {
        sr |= SR_RFF;
    }
    return sr;
}

static void lpc1778_ssp_xfer(Lpc1778SspState *s, uint32_t val)
{
    uint32_t rx;

    if (!(s->cr1 & CR1_SSE)) {
        return;
    }
    val &= lpc1778_ssp_mask(s);
    rx = (s->cr1 & CR1_LBM) ? val : ssi_transfer(s->ssi, val);
    rx &= lpc1778_ssp_mask(s);
    if (s->rx_len < LPC1778_SSP_FIFO) {
        s->rx_fifo[s->rx_head] = (uint16_t)rx;
        s->rx_head = (s->rx_head + 1) % LPC1778_SSP_FIFO;
        s->rx_len++;
    } else {
        /* UM10470 21.6.6: the word is lost and RORRIS goes up. */
        s->ris |= RIS_ROR;
    }
    lpc1778_ssp_update_irq(s);
}

static uint32_t lpc1778_ssp_pop(Lpc1778SspState *s)
{
    uint32_t val;

    if (!s->rx_len) {
        return 0;
    }
    val = s->rx_fifo[s->rx_tail];
    s->rx_tail = (s->rx_tail + 1) % LPC1778_SSP_FIFO;
    s->rx_len--;
    lpc1778_ssp_update_irq(s);
    return val;
}

static uint64_t lpc1778_ssp_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778SspState *s = opaque;

    (void)size;
    switch (addr) {
    case REG_CR0:
        return s->cr0;
    case REG_CR1:
        return s->cr1;
    case REG_DR:
        return lpc1778_ssp_pop(s);
    case REG_SR:
        return lpc1778_ssp_status(s);
    case REG_CPSR:
        return s->cpsr;
    case REG_IMSC:
        return s->imsc;
    case REG_RIS:
        return s->ris;
    case REG_MIS:
        return s->ris & s->imsc;
    case REG_DMACR:
        return s->dmacr;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: read 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
}

static void lpc1778_ssp_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778SspState *s = opaque;

    (void)size;
    switch (addr) {
    case REG_CR0:
        s->cr0 = value & 0xFFFFu;
        break;
    case REG_CR1:
        s->cr1 = value & 0xFu;
        lpc1778_ssp_update_irq(s);
        break;
    case REG_DR:
        lpc1778_ssp_xfer(s, value);
        break;
    case REG_CPSR:
        s->cpsr = value & 0xFFu;
        break;
    case REG_IMSC:
        s->imsc = value & 0xFu;
        lpc1778_ssp_update_irq(s);
        break;
    case REG_ICR:
        /* Write-1-to-clear, overrun and receive timeout only. */
        s->ris &= ~(uint32_t)(value & (RIS_ROR | RIS_RT));
        lpc1778_ssp_update_irq(s);
        break;
    case REG_DMACR:
        s->dmacr = value & 3u;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: write 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
}

static const MemoryRegionOps lpc1778_ssp_ops = {
    .read = lpc1778_ssp_read,
    .write = lpc1778_ssp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void lpc1778_ssp_reset(DeviceState *dev)
{
    Lpc1778SspState *s = LPC1778_SSP(dev);

    s->cr0 = 0;
    s->cr1 = 0;
    s->cpsr = 0;
    s->imsc = 0;
    s->ris = RIS_TX;
    s->dmacr = 0;
    s->rx_head = 0;
    s->rx_tail = 0;
    s->rx_len = 0;
    memset(s->rx_fifo, 0, sizeof(s->rx_fifo));
    qemu_set_irq(s->irq, 0);
}

static void lpc1778_ssp_init(Object *obj)
{
    Lpc1778SspState *s = LPC1778_SSP(obj);
    DeviceState *dev = DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &lpc1778_ssp_ops, s,
                          TYPE_LPC1778_SSP, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    s->ssi = ssi_create_bus(dev, "ssi");
}

static const VMStateDescription vmstate_lpc1778_ssp = {
    .name = TYPE_LPC1778_SSP,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(cr0, Lpc1778SspState),
        VMSTATE_UINT32(cr1, Lpc1778SspState),
        VMSTATE_UINT32(cpsr, Lpc1778SspState),
        VMSTATE_UINT32(imsc, Lpc1778SspState),
        VMSTATE_UINT32(ris, Lpc1778SspState),
        VMSTATE_UINT32(dmacr, Lpc1778SspState),
        VMSTATE_UINT16_ARRAY(rx_fifo, Lpc1778SspState, LPC1778_SSP_FIFO),
        VMSTATE_INT32(rx_head, Lpc1778SspState),
        VMSTATE_INT32(rx_tail, Lpc1778SspState),
        VMSTATE_INT32(rx_len, Lpc1778SspState),
        VMSTATE_END_OF_LIST()
    }
};

static void lpc1778_ssp_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_ssp_reset);
    dc->vmsd = &vmstate_lpc1778_ssp;
}

static const TypeInfo lpc1778_ssp_info = {
    .name = TYPE_LPC1778_SSP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778SspState),
    .instance_init = lpc1778_ssp_init,
    .class_init = lpc1778_ssp_class_init,
};

static void lpc1778_ssp_register_types(void)
{
    type_register_static(&lpc1778_ssp_info);
}

type_init(lpc1778_ssp_register_types)
