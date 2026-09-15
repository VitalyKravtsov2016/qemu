/*
 * NXP LPC1778 USB device controller (UM10470) — init stub.
 *
 * Firmware @0x1A59C enables USB clocks then selects the port:
 *   USBClkCtrl = 0x12/0x1A; while ((USBClkSt & mask) != mask);
 *   USBPortSel = 3;
 *
 * Also provides enough DevInt/SIE/EP realize behaviour for later init.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/lpc1778_usb.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "qemu/module.h"

#define REG_PORTSEL   0x110
#define REG_DEVINTST  0x200
#define REG_DEVINTEN  0x204
#define REG_DEVINTCLR 0x208
#define REG_DEVINTSET 0x20C
#define REG_CMDCODE   0x210
#define REG_CMDDATA   0x214
#define REG_REEP      0x244
#define REG_EPIND     0x248
#define REG_MAXPSIZE  0x24C
#define REG_CLKCTRL   0xFF4
#define REG_CLKST     0xFF8

#define DEVINT_CCEMPTY (1u << 4)
#define DEVINT_CDFULL  (1u << 5)
#define DEVINT_EP_RLZED (1u << 8)

#define CMD_PHASE_WRITE 0x01
#define CMD_PHASE_READ  0x02
#define CMD_PHASE_CMD   0x05

#define SIE_READ_TEST_REG 0xFD

static uint32_t *regp(Lpc1778UsbState *s, uint32_t off)
{
    return &s->regs[off / 4];
}

static void sie_command(Lpc1778UsbState *s, uint32_t value)
{
    uint8_t phase = (value >> 8) & 0xff;
    uint8_t code = (value >> 16) & 0xff;

    *regp(s, REG_DEVINTST) |= DEVINT_CCEMPTY;

    if (phase == CMD_PHASE_READ) {
        uint32_t data = 0;

        if (code == SIE_READ_TEST_REG) {
            data = 0xA50F;
        }
        *regp(s, REG_CMDDATA) = data;
        *regp(s, REG_DEVINTST) |= DEVINT_CDFULL;
    } else if (phase == CMD_PHASE_WRITE || phase == CMD_PHASE_CMD) {
        /* Instant completion; CCEMPTY already set. */
    }
}

static uint64_t lpc1778_usb_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778UsbState *s = opaque;
    uint32_t off = addr & ~3u;

    if (off >= LPC1778_USB_SIZE) {
        return 0;
    }
    if (off == REG_CLKST) {
        /* Clocks become available as soon as the enable bits are written. */
        return *regp(s, REG_CLKCTRL);
    }
    return *regp(s, off);
}

static void lpc1778_usb_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778UsbState *s = opaque;
    uint32_t off = addr & ~3u;
    uint32_t v = (uint32_t)value;

    if (off >= LPC1778_USB_SIZE) {
        return;
    }

    switch (off) {
    case REG_DEVINTCLR:
        *regp(s, REG_DEVINTST) &= ~v;
        /* CCEMPTY is sticky when the command engine is idle. */
        *regp(s, REG_DEVINTST) |= DEVINT_CCEMPTY;
        break;
    case REG_DEVINTSET:
        *regp(s, REG_DEVINTST) |= v;
        break;
    case REG_CMDCODE:
        sie_command(s, v);
        *regp(s, off) = v;
        break;
    case REG_REEP:
    case REG_MAXPSIZE:
        *regp(s, off) = v;
        *regp(s, REG_DEVINTST) |= DEVINT_EP_RLZED;
        break;
    case REG_CLKST:
        /* Read-only */
        break;
    case REG_DEVINTST:
        /* Read-only */
        break;
    case REG_CMDDATA:
        /* Read-only */
        break;
    default:
        *regp(s, off) = v;
        break;
    }
}

static const MemoryRegionOps lpc1778_usb_ops = {
    .read = lpc1778_usb_read,
    .write = lpc1778_usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_usb_reset(DeviceState *dev)
{
    Lpc1778UsbState *s = LPC1778_USB(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* UM10470: USBDevIntSt reset value 0x10 (CCEMPTY). */
    *regp(s, REG_DEVINTST) = DEVINT_CCEMPTY;
    qemu_set_irq(s->irq, 0);
}

static void lpc1778_usb_init(Object *obj)
{
    Lpc1778UsbState *s = LPC1778_USB(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_usb_ops, s,
                          TYPE_LPC1778_USB, LPC1778_USB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_usb_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_usb_reset);
}

static const TypeInfo lpc1778_usb_info = {
    .name = TYPE_LPC1778_USB,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778UsbState),
    .instance_init = lpc1778_usb_init,
    .class_init = lpc1778_usb_class_init,
};

static void lpc1778_usb_register_types(void)
{
    type_register_static(&lpc1778_usb_info);
}

type_init(lpc1778_usb_register_types)
