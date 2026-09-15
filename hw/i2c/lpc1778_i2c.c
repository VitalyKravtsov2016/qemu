/*
 * NXP LPC1778 I2C master (I2C0 @ 0x4001C000 / I2C1 @ 0x4005C000).
 * I2C0: FN slave @ addr 2. I2C1: same block; unknown addresses NACK.
 * Hardware-only: Описание-ФН-1-2 frames via FnSlave; busy address NACKs.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/lpc1778_i2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qemu/module.h"

#define REG_CONSET 0x00
#define REG_STAT   0x04
#define REG_DAT    0x08
#define REG_ADR0   0x0C
#define REG_SCLH   0x10
#define REG_SCLL   0x14
#define REG_CONCLR 0x18

#define CON_AA   (1u << 2)
#define CON_SI   (1u << 3)
#define CON_STO  (1u << 4)
#define CON_STA  (1u << 5)
#define CON_I2EN (1u << 6)

#define FN_ADDR7 2

static void lpc1778_i2c_update_irq(Lpc1778I2cState *s)
{
    qemu_set_irq(s->irq, (s->con & CON_SI) != 0);
}

static void lpc1778_i2c_set_si(Lpc1778I2cState *s, uint32_t st)
{
    s->stat = st;
    s->con |= CON_SI;
    lpc1778_i2c_update_irq(s);
}

static void lpc1778_i2c_clear_si(Lpc1778I2cState *s)
{
    s->con &= ~CON_SI;
    lpc1778_i2c_update_irq(s);
}

static void lpc1778_i2c_handle_start(Lpc1778I2cState *s)
{
    s->con &= ~CON_STO;
    s->phase = I2C_START;
    s->pending_dat = false;
    s->tx_len = 0;
    lpc1778_i2c_set_si(s, 0x08);
}

static void lpc1778_i2c_handle_stop(Lpc1778I2cState *s)
{
    if (s->phase == I2C_TX || s->phase == I2C_ADDR_W) {
        fn_slave_on_master_write(&s->fn, s->tx, s->tx_len);
    }
    s->phase = I2C_IDLE;
    s->con &= ~(CON_STO | CON_STA | CON_SI);
    s->stat = 0xF8;
    s->pending_dat = false;
    s->tx_len = 0;
    lpc1778_i2c_update_irq(s);
}

static void lpc1778_i2c_do_continue(Lpc1778I2cState *s)
{
    lpc1778_i2c_clear_si(s);

    if (s->pending_dat) {
        uint8_t v = s->pending_byte;
        s->pending_dat = false;
        s->dat = v;

        if (s->phase == I2C_START) {
            bool read = (v & 1u) != 0;
            uint8_t addr7 = v >> 1;
            if (!s->fn_slave || addr7 != FN_ADDR7) {
                lpc1778_i2c_set_si(s, read ? 0x48 : 0x20);
                s->phase = I2C_IDLE;
                return;
            }
            if (fn_slave_busy(&s->fn)) {
                fn_slave_note_address_probe(&s->fn, read);
                lpc1778_i2c_set_si(s, read ? 0x48 : 0x20);
                s->phase = I2C_IDLE;
                return;
            }
            fn_slave_note_address_probe(&s->fn, read);
            if (read) {
                s->phase = I2C_ADDR_R;
                lpc1778_i2c_set_si(s, 0x40);
            } else {
                s->phase = I2C_ADDR_W;
                lpc1778_i2c_set_si(s, 0x18);
            }
            return;
        }
        if (s->phase == I2C_ADDR_W || s->phase == I2C_TX) {
            s->phase = I2C_TX;
            if (s->tx_len < sizeof(s->tx)) {
                s->tx[s->tx_len++] = v;
            }
            lpc1778_i2c_set_si(s, 0x28);
            return;
        }
        lpc1778_i2c_set_si(s, 0x28);
        return;
    }

    if (s->phase == I2C_ADDR_R || s->phase == I2C_RX) {
        uint8_t b = 0;
        s->phase = I2C_RX;
        if (!fn_slave_pop_rx(&s->fn, &b)) {
            b = 0xFF;
        }
        s->dat = b;
        lpc1778_i2c_set_si(s, s->aa ? 0x50 : 0x58);
    }
}

static uint64_t lpc1778_i2c_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778I2cState *s = opaque;

    switch (addr) {
    case REG_CONSET:
        return s->con;
    case REG_STAT:
        return s->stat;
    case REG_DAT:
        return s->dat;
    case REG_ADR0:
        return s->adr0;
    case REG_SCLH:
        return s->sclh;
    case REG_SCLL:
        return s->scll;
    default:
        return 0;
    }
}

static void lpc1778_i2c_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778I2cState *s = opaque;
    uint32_t v = value;

    switch (addr) {
    case REG_CONSET:
        s->con |= v & (CON_AA | CON_STO | CON_STA | CON_I2EN);
        if (v & CON_AA) {
            s->aa = true;
        }
        if (v & CON_STA) {
            lpc1778_i2c_handle_start(s);
        }
        if (v & CON_STO) {
            lpc1778_i2c_handle_stop(s);
        }
        break;
    case REG_CONCLR:
        if (v & CON_AA) {
            s->con &= ~CON_AA;
            s->aa = false;
        }
        if (v & CON_STA) {
            s->con &= ~CON_STA;
        }
        if (v & CON_I2EN) {
            s->con &= ~CON_I2EN;
        }
        if (v & CON_SI) {
            lpc1778_i2c_do_continue(s);
        }
        break;
    case REG_DAT:
        s->pending_byte = v;
        s->pending_dat = true;
        break;
    case REG_ADR0:
        s->adr0 = v;
        break;
    case REG_SCLH:
        s->sclh = v;
        break;
    case REG_SCLL:
        s->scll = v;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps lpc1778_i2c_ops = {
    .read = lpc1778_i2c_read,
    .write = lpc1778_i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_i2c_reset(DeviceState *dev)
{
    Lpc1778I2cState *s = LPC1778_I2C(dev);

    s->con = 0;
    s->stat = 0xF8;
    s->dat = 0;
    s->adr0 = 0;
    s->sclh = s->scll = 0;
    s->phase = I2C_IDLE;
    s->aa = false;
    s->pending_dat = false;
    s->tx_len = 0;
    fn_slave_reset(&s->fn);
    lpc1778_i2c_update_irq(s);
}

static void lpc1778_i2c_realize(DeviceState *dev, Error **errp)
{
    Lpc1778I2cState *s = LPC1778_I2C(dev);

    (void)errp;
    if (s->fn_slave && s->fn_log_path && s->fn_log_path[0]) {
        fn_slave_set_log_path(&s->fn, s->fn_log_path);
    }
    if (s->fn_slave && s->fn_persist_path && s->fn_persist_path[0]) {
        fn_slave_set_persist_path(&s->fn, s->fn_persist_path);
    }
}

static void lpc1778_i2c_finalize(Object *obj)
{
    Lpc1778I2cState *s = LPC1778_I2C(obj);

    fn_slave_flush(&s->fn);
    fn_slave_close_log(&s->fn);
    g_free(s->fn.persist_path);
    s->fn.persist_path = NULL;
}

static void lpc1778_i2c_init(Object *obj)
{
    Lpc1778I2cState *s = LPC1778_I2C(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_i2c_ops, s,
                          TYPE_LPC1778_I2C, 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    fn_slave_cold_init(&s->fn);
}

static const Property lpc1778_i2c_properties[] = {
    DEFINE_PROP_BOOL("fn-slave", Lpc1778I2cState, fn_slave, false),
    DEFINE_PROP_STRING("fn-log-path", Lpc1778I2cState, fn_log_path),
    DEFINE_PROP_STRING("fn-persist-path", Lpc1778I2cState, fn_persist_path),
};

static void lpc1778_i2c_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = lpc1778_i2c_realize;
    device_class_set_legacy_reset(dc, lpc1778_i2c_reset);
    device_class_set_props(dc, lpc1778_i2c_properties);
}

static const TypeInfo lpc1778_i2c_info = {
    .name = TYPE_LPC1778_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778I2cState),
    .instance_init = lpc1778_i2c_init,
    .instance_finalize = lpc1778_i2c_finalize,
    .class_init = lpc1778_i2c_class_init,
};

static void lpc1778_i2c_register_types(void)
{
    type_register_static(&lpc1778_i2c_info);
}

type_init(lpc1778_i2c_register_types)
