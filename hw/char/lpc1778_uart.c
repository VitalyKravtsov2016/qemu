/*
 * NXP LPC1778 UART (16x50-compatible) — RX FIFO + THRE IRQ.
 *
 * Board (SME16031.100.00): UART0 TXD/RXD (P0.2 / P0.3) through TRS3232E
 * to XP18 (DB-9). The PHY delivers one 8N1 character per baud period,
 * not a TCP burst into the 16-byte FIFO.
 *
 * QEMU's v7M NVIC latches external IRQs on a 0→1 edge (and again on
 * handler return if the line is still high). Holding the line while the
 * FIFO is stuffed tail-chains UART0 above PendSV. Pulse each RDA/THRE.
 * Do not wait a baud period per octet: under icount that stalls TCP and
 * the host injects ENQ into a still-open STX frame.
 *
 * Writing THR clears the THRE *interrupt*. IER.THRE 0→1 with THR empty
 * raises THRE immediately — the firmware starts an STX reply that way
 * (state 6, then IER bit1) and does not write THR first.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/char/lpc1778_uart.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define RBR_THR_DLL 0x00
#define IER_DLM     0x04
#define IIR_FCR     0x08
#define LCR         0x0C
#define MCR         0x10
#define LSR         0x14
#define MSR         0x18
#define SCR         0x1C
#define FDR         0x28
#define TER         0x30
#define ACR         0x20
#define ICR         0x24
#define UART_RS485CTRL 0x4C
#define UART_RS485ADRMATCH 0x50
#define UART_RS485DLY 0x54

#define LSR_RDR  (1u << 0)
#define LSR_THRE (1u << 5)
#define LSR_TEMT (1u << 6)
#define LCR_DLAB (1u << 7)
#define IER_RDA  (1u << 0)
#define IER_THRE (1u << 1)
#define IIR_NOINT 0x01
#define IIR_THRI  0x02
#define IIR_RDA   0x04
#define IIR_FE    0xC0
#define FCR_FE    (1u << 0)

static bool lpc1778_uart_irq_pending(Lpc1778UartState *s)
{
    uint8_t fifo_bits = (s->fcr & FCR_FE) ? IIR_FE : 0;

    if ((s->ier & IER_RDA) && s->rx_len) {
        s->iir = IIR_RDA | fifo_bits;
        return true;
    }
    if ((s->ier & IER_THRE) && s->thr_ipending) {
        s->iir = IIR_THRI | fifo_bits;
        return true;
    }
    s->iir = IIR_NOINT | fifo_bits;
    return false;
}

static void lpc1778_uart_pulse_irq(Lpc1778UartState *s)
{
    bool pending = lpc1778_uart_irq_pending(s);

    if (s->diag) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "lpc1778-uart[%p]: IRQ pending=%d IIR=%02x IER=%02x "
                      "rx=%u thre=%d\n",
                      (void *)s, pending, s->iir, s->ier, s->rx_len,
                      s->thr_ipending);
    }
    if (pending) {
        qemu_set_irq(s->irq, 1);
        qemu_set_irq(s->irq, 0);
    } else {
        qemu_set_irq(s->irq, 0);
    }
}

static void lpc1778_uart_irq_bh(void *opaque)
{
    /* One pulse per schedule. Do not rearm here: a self-rescheduling BH
     * starves TCG (aio_bh_poll never returns). */
    lpc1778_uart_pulse_irq(opaque);
}

static void lpc1778_uart_update_irq(Lpc1778UartState *s)
{
    lpc1778_uart_irq_pending(s);
    qemu_bh_schedule(s->irq_bh);
}

static void lpc1778_uart_thre_tick(void *opaque)
{
    Lpc1778UartState *s = opaque;

    s->lsr |= LSR_THRE | LSR_TEMT;
    if (s->ier & IER_THRE) {
        s->thr_ipending = true;
        lpc1778_uart_pulse_irq(s);
    }
}

static void lpc1778_uart_arm_thre(Lpc1778UartState *s)
{
    /* UM10470: writing THR clears the THRE interrupt. Re-raise on the
     * next virtual tick (not a baud delay) so extra THRE @0x4c1b6 still
     * runs after the TX ISR and before a TCP host ACK. */
    s->thr_ipending = false;
    if (s->thre_timer) {
        timer_mod_ns(s->thre_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1);
    }
}

static void lpc1778_uart_rx_tick(void *opaque)
{
    Lpc1778UartState *s = opaque;

    qemu_chr_fe_accept_input(&s->chr);
}

static int lpc1778_uart_can_receive(void *opaque)
{
    Lpc1778UartState *s = opaque;

    if (s->rx_len >= LPC1778_UART_FIFO) {
        return 0;
    }
    return 1;
}

static void lpc1778_uart_receive(void *opaque, const uint8_t *buf, int size)
{
    Lpc1778UartState *s = opaque;
    unsigned w;

    if (size < 1 || s->rx_len >= LPC1778_UART_FIFO) {
        return;
    }
    w = (s->rx_r + s->rx_len) % LPC1778_UART_FIFO;
    s->rx_fifo[w] = buf[0];
    s->rx_len++;
    s->lsr |= LSR_RDR;
    if (s->diag) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "lpc1778-uart[%p]: RX %02x depth=%u\n",
                      (void *)s, buf[0], s->rx_len);
    }
    lpc1778_uart_update_irq(s);
}

static uint8_t lpc1778_uart_pop_rx(Lpc1778UartState *s)
{
    uint8_t b = 0;

    if (s->rx_len) {
        b = s->rx_fifo[s->rx_r];
        s->rx_r = (s->rx_r + 1) % LPC1778_UART_FIFO;
        s->rx_len--;
    }
    if (!s->rx_len) {
        s->lsr &= ~LSR_RDR;
    }
    if (s->diag) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "lpc1778-uart[%p]: RBR -> %02x depth=%u\n",
                      (void *)s, b, s->rx_len);
    }
    qemu_chr_fe_accept_input(&s->chr);
    lpc1778_uart_update_irq(s);
    return b;
}

static uint64_t lpc1778_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778UartState *s = opaque;
    uint8_t iir;

    switch (addr) {
    case RBR_THR_DLL:
        if (s->lcr & LCR_DLAB) {
            return s->dll;
        }
        return lpc1778_uart_pop_rx(s);
    case IER_DLM:
        return (s->lcr & LCR_DLAB) ? s->dlm : s->ier;
    case IIR_FCR:
        lpc1778_uart_irq_pending(s);
        iir = s->iir;
        if (s->diag) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "lpc1778-uart[%p]: IIR -> %02x\n",
                          (void *)s, iir);
        }
        /* Reading IIR when THRE is the current source clears that interrupt. */
        if ((iir & 0x0F) == IIR_THRI) {
            s->thr_ipending = false;
            lpc1778_uart_update_irq(s);
        }
        return iir;
    case LCR:
        return s->lcr;
    case MCR:
        return s->mcr;
    case LSR:
        qemu_chr_fe_accept_input(&s->chr);
        /* Poll-TX (@0x4c058) busy-waits LSR.THRE; QEMU timers do not run
         * inside that TCG loop, so keep THRE readable. IIR.THRE stays
         * paced by thre_timer. */
        return s->lsr | LSR_THRE | LSR_TEMT;
    case MSR:
        return s->msr;
    case SCR:
        return s->scr;
    case FDR:
        return s->fdr;
    case TER:
        return s->ter;
    case ACR:
    case ICR:
    case UART_RS485CTRL:
    case UART_RS485ADRMATCH:
    case UART_RS485DLY:
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        return 0;
    }
}

static void lpc1778_uart_write(void *opaque, hwaddr addr,
                               uint64_t value, unsigned size)
{
    Lpc1778UartState *s = opaque;
    uint8_t ch;
    uint8_t changed;

    switch (addr) {
    case RBR_THR_DLL:
        if (s->lcr & LCR_DLAB) {
            s->dll = value;
            return;
        }
        ch = value;
        if (s->diag) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "lpc1778-uart[%p]: THR <- %02x\n",
                          (void *)s, ch);
        }
        qemu_chr_fe_write_all(&s->chr, &ch, 1);
        s->lsr |= LSR_THRE | LSR_TEMT;
        lpc1778_uart_arm_thre(s);
        lpc1778_uart_update_irq(s);
        return;
    case IER_DLM:
        if (s->lcr & LCR_DLAB) {
            s->dlm = value;
            return;
        }
        changed = (s->ier ^ (uint8_t)value) & 0x0F;
        if (s->diag) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "lpc1778-uart[%p]: IER %02x -> %02x\n",
                          (void *)s, s->ier, (uint8_t)value & 0x0F);
        }
        s->ier = value & 0x0F;
        if (changed & IER_THRE) {
            if (s->ier & IER_THRE) {
                /*
                 * UM10470: IER.THRE 0→1 with THR empty raises THRE.
                 * Firmware starts an STX reply by setting state 6 and
                 * enabling this bit — it does not poke THR first.
                 */
                s->thr_ipending = true;
            } else {
                s->thr_ipending = false;
            }
        }
        lpc1778_uart_update_irq(s);
        return;
    case IIR_FCR:
        s->fcr = value;
        if (value & 0x02) {
            s->rx_len = 0;
            s->rx_r = 0;
            s->lsr &= ~LSR_RDR;
        }
        lpc1778_uart_update_irq(s);
        return;
    case LCR:
        s->lcr = value;
        return;
    case MCR:
        s->mcr = value;
        return;
    case SCR:
        s->scr = value;
        return;
    case FDR:
        s->fdr = value;
        return;
    case TER:
        s->ter = value;
        return;
    case ACR:
    case ICR:
    case UART_RS485CTRL:
    case UART_RS485ADRMATCH:
    case UART_RS485DLY:
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
    }
}

static const MemoryRegionOps lpc1778_uart_ops = {
    .read = lpc1778_uart_read,
    .write = lpc1778_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_uart_reset(DeviceState *dev)
{
    Lpc1778UartState *s = LPC1778_UART(dev);

    s->rbr = 0;
    s->ier = 0;
    s->iir = IIR_NOINT;
    s->lcr = 0;
    s->mcr = 0;
    s->lsr = LSR_THRE | LSR_TEMT;
    s->msr = 0;
    s->scr = 0;
    s->dll = 1;
    s->dlm = 0;
    s->fcr = 0;
    s->fdr = 0x10;
    s->ter = 0x80;
    s->rx_len = 0;
    s->rx_r = 0;
    s->thr_ipending = false;
    s->tx_shift = 0;
    s->tx_shift_busy = false;
    if (s->thre_timer) {
        timer_del(s->thre_timer);
    }
    if (s->rx_timer) {
        timer_del(s->rx_timer);
    }
    lpc1778_uart_update_irq(s);
}

static void lpc1778_uart_init(Object *obj)
{
    Lpc1778UartState *s = LPC1778_UART(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_uart_ops, s,
                          TYPE_LPC1778_UART, 0x80);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->irq_bh = qemu_bh_new(lpc1778_uart_irq_bh, s);
    s->thre_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lpc1778_uart_thre_tick, s);
    s->rx_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, lpc1778_uart_rx_tick, s);
}

static void lpc1778_uart_finalize(Object *obj)
{
    Lpc1778UartState *s = LPC1778_UART(obj);

    qemu_bh_delete(s->irq_bh);
    s->irq_bh = NULL;
    if (s->thre_timer) {
        timer_free(s->thre_timer);
        s->thre_timer = NULL;
    }
    if (s->rx_timer) {
        timer_free(s->rx_timer);
        s->rx_timer = NULL;
    }
}

static void lpc1778_uart_realize(DeviceState *dev, Error **errp)
{
    Lpc1778UartState *s = LPC1778_UART(dev);

    qemu_chr_fe_set_handlers(&s->chr, lpc1778_uart_can_receive,
                             lpc1778_uart_receive, NULL, NULL, s, NULL, true);
}

static const Property lpc1778_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", Lpc1778UartState, chr),
    DEFINE_PROP_BOOL("diag", Lpc1778UartState, diag, false),
};

static void lpc1778_uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_uart_reset);
    device_class_set_props(dc, lpc1778_uart_properties);
    dc->realize = lpc1778_uart_realize;
}

static const TypeInfo lpc1778_uart_info = {
    .name = TYPE_LPC1778_UART,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778UartState),
    .instance_init = lpc1778_uart_init,
    .instance_finalize = lpc1778_uart_finalize,
    .class_init = lpc1778_uart_class_init,
};

static void lpc1778_uart_register_types(void)
{
    type_register_static(&lpc1778_uart_info);
}

type_init(lpc1778_uart_register_types)
