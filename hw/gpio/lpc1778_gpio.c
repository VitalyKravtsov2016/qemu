/*
 * NXP LPC1778 GPIO (ports 0..5) + SHTRIH-M-01F sensor inputs.
 *
 * Schematic SME16031.100.00 (РемДок sheet MCU / sensors / mech):
 *   CP_COVER_UP  P0.8   pull-up 10k, switch to GND (XP9) — high = cover up
 *   KEY          P0.9   feed button; firmware samples pressed = high
 *                       (QOM feed-pressed true → pin high)
 *   DK_SEN       P0.10  drawer: QOM closed → pin high (host polarity)
 *   TPE          P0.12  also AD0[6]; phototransistor pulls the net down when
 *                       paper covers the optics, so QOM present → pin low.
 *                       Measured on 10h/11h: flag "оптический датчик ЧЛ"
 *                       (bit 7) is set only while TPE reads low.
 *   TPNE1        P0.13  also AD0[7]; same active-low optics, QOM near-end →
 *                       pin low (flag "рулон ЧЛ", bit 1)
 *   TPNE2        P0.25  a second physical sensor on its own net; both TPNE
 *                       inputs still follow the single paper-near-end prop.
 *                       nTPNE1_ON (DA6) gating of sensor power is not modelled.
 *   nFAULT       P0.1   DRV8800 fault (idle high)
 *   AC_SEN       P0.4   cutter home
 *   STEP_*       P2.0–5 BD63510 (also PWM1[1..6])
 *   Printer ID   P3.22–31: P3.27 tied to GND, other ID bits NC (pull-up).
 *                Firmware @0x42d24 reads ~FIO3PIN>>22 and selects the mech table.
 *
 * QOM (path /machine/soc/gpio), live via QMP qom-set:
 *   paper-present, paper-near-end, cover-closed, feed-pressed, drawer-closed
 * Read-only: led1, led2 (P1.27 / P1.28).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/gpio/lpc1778_gpio.h"
#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* Schematic SME16031 → MCU port 0 */
#define BIT_COVER_UP   8
#define BIT_FEED_KEY   9   /* firmware: pressed = high (board active-high to MCU) */
#define BIT_DRAWER     10
#define BIT_TPE        12  /* paper present → pin high */
#define BIT_TPNE1      13  /* near-end → pin high */
#define BIT_TPNE2      25
#define BIT_AC_SEN     4
#define BIT_NFAULT     1
#define BIT_SDA        27
#define BIT_SCL        28
#define BIT_ISPMODE    10  /* port 2 */
#define BIT_LED1       27  /* port 1 */
#define BIT_LED2       28
#define BIT_P3_ID_GND  27  /* P3.27 strap to GND */

/*
 * Detect uses FIO3 bits 22..31 only. NC pads read high (pull-up);
 * P3.27 is wired to GND → ID 0x20 → printer type 4 (live GPIO methods).
 */
#define P3_ID_MASK     (0x3FFu << 22)
#define P3_ID_PULLUP   (P3_ID_MASK & ~(1u << BIT_P3_ID_GND))

/* Pads driven by board circuits, not by the MCU (inputs). */
#define P0_BOARD_IN                                                        \
    ((1u << BIT_NFAULT) | (1u << BIT_AC_SEN) | (1u << BIT_COVER_UP) |      \
     (1u << BIT_FEED_KEY) | (1u << BIT_DRAWER) | (1u << BIT_TPE) |         \
     (1u << BIT_TPNE1) | (1u << BIT_TPNE2) |                               \
     (1u << BIT_SDA) | (1u << BIT_SCL))

static int port_of(hwaddr addr)
{
    return addr / 0x20;
}

static uint32_t lane_mask(unsigned size, unsigned shift)
{
    uint32_t w;

    if (size == 1) {
        w = 0xffu;
    } else if (size == 2) {
        w = 0xffffu;
    } else {
        w = 0xffffffffu;
    }
    return w << shift;
}

static void lpc1778_gpio_notify(Lpc1778GpioState *s)
{
    int i;

    for (i = 0; i < s->nchange; i++) {
        if (s->change[i]) {
            s->change[i](s->change_opaque[i]);
        }
    }
}

static void set_in_bit(Lpc1778GpioState *s, unsigned port, unsigned bit, bool high)
{
    uint32_t m = 1u << bit;

    if (high) {
        s->in[port] |= m;
    } else {
        s->in[port] &= ~m;
    }
}

void lpc1778_gpio_set_in_bit(Lpc1778GpioState *s, unsigned port,
                             unsigned bit, bool high)
{
    uint32_t old;

    if (port >= LPC1778_GPIO_PORTS || bit >= 32) {
        return;
    }
    old = s->in[port];
    set_in_bit(s, port, bit, high);
    if (s->in[port] != old) {
        lpc1778_gpio_notify(s);
    }
}

void lpc1778_gpio_apply_board(Lpc1778GpioState *s)
{
    /* ISPMODE high = run firmware (not DFU jumper). */
    set_in_bit(s, 2, BIT_ISPMODE, true);
    /* I2C0 FN pull-ups, idle high. */
    set_in_bit(s, 0, BIT_SDA, true);
    set_in_bit(s, 0, BIT_SCL, true);
    /*
     * AC_SEN / nFAULT are driven by the cutter/stepper model. Do not force
     * them home/ok here: QOM sensor updates would cancel an in-progress cut
     * and the print ISR would never see home (submode 5).
     */

    /* Optics are active low: paper/roll in the slot pulls TPE / TPNE down. */
    set_in_bit(s, 0, BIT_TPE, !s->paper_present);
    set_in_bit(s, 0, BIT_TPNE1, !s->paper_near_end);
    set_in_bit(s, 0, BIT_TPNE2, !s->paper_near_end);
    set_in_bit(s, 0, BIT_COVER_UP, !s->cover_closed);
    set_in_bit(s, 0, BIT_DRAWER, s->drawer_closed);
    set_in_bit(s, 0, BIT_FEED_KEY, s->feed_pressed);

    /* Printer-type ID straps on EMC D22..D31 (P3). */
    s->in[3] = (s->in[3] & ~P3_ID_MASK) | P3_ID_PULLUP;
}

#define PROP_BOOL(name, field)                                                 \
static bool lpc1778_gpio_get_##field(Object *obj, Error **errp)                \
{                                                                              \
    Lpc1778GpioState *s = LPC1778_GPIO(obj);                                   \
    (void)errp;                                                                \
    return s->field;                                                           \
}                                                                              \
static void lpc1778_gpio_set_##field(Object *obj, bool value, Error **errp)    \
{                                                                              \
    Lpc1778GpioState *s = LPC1778_GPIO(obj);                                   \
    (void)errp;                                                                \
    s->field = value;                                                          \
    lpc1778_gpio_apply_board(s);                                               \
    lpc1778_gpio_notify(s);                                                    \
}

PROP_BOOL("paper-present", paper_present)
PROP_BOOL("paper-near-end", paper_near_end)
PROP_BOOL("cover-closed", cover_closed)
PROP_BOOL("feed-pressed", feed_pressed)
PROP_BOOL("drawer-closed", drawer_closed)

#undef PROP_BOOL

static bool lpc1778_gpio_get_led1(Object *obj, Error **errp)
{
    Lpc1778GpioState *s = LPC1778_GPIO(obj);

    (void)errp;
    return (s->out[1] & (1u << BIT_LED1)) != 0;
}

static bool lpc1778_gpio_get_led2(Object *obj, Error **errp)
{
    Lpc1778GpioState *s = LPC1778_GPIO(obj);

    (void)errp;
    return (s->out[1] & (1u << BIT_LED2)) != 0;
}

uint32_t lpc1778_gpio_get_pins(const Lpc1778GpioState *s, int p)
{
    uint32_t v = (s->out[p] & s->dir[p]) | (s->in[p] & ~s->dir[p]);

    /*
     * UM10470 9.5.4: PIN reflects the physical pad, regardless of direction,
     * unless the pin is an analog ADC input. Board sensors are external
     * drivers (pull-up + switch / phototransistor / VT17), so keep those
     * levels even if DIR was set as GPIO output.
     */
    if (p == 0) {
        v = (v & ~P0_BOARD_IN) | (s->in[0] & P0_BOARD_IN);
    } else if (p == 2) {
        v = (v & ~(1u << BIT_ISPMODE)) | (s->in[2] & (1u << BIT_ISPMODE));
    } else if (p == 3) {
        v = (v & ~P3_ID_MASK) | (s->in[3] & P3_ID_MASK);
    }

    return v;
}

static uint32_t lpc1778_gpio_reg32(const Lpc1778GpioState *s, int p, uint32_t off)
{
    switch (off) {
    case 0x00:
        return s->dir[p];
    case 0x10:
        return s->mask[p];
    case 0x14:
        return lpc1778_gpio_get_pins(s, p);
    case 0x18:
        /* CMSIS and this firmware read SET as the pin sample, like PIN. */
        return lpc1778_gpio_get_pins(s, p);
    default:
        return 0;
    }
}

static uint64_t lpc1778_gpio_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778GpioState *s = opaque;
    int p = port_of(addr);
    unsigned shift;
    uint32_t wmask;

    if (p < 0 || p >= LPC1778_GPIO_PORTS) {
        return 0;
    }
    /* Byte/half-word windows: FIO0PIN1 @ +0x15, FIO0PINL @ +0x14, … */
    shift = (addr & 3u) * 8u;
    wmask = lane_mask(size, 0);
    return (lpc1778_gpio_reg32(s, p, addr & 0x1C) >> shift) & wmask;
}

static void lpc1778_gpio_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778GpioState *s = opaque;
    int p = port_of(addr);
    uint32_t off, shift, lane, bits, affect;

    if (p < 0 || p >= LPC1778_GPIO_PORTS) {
        return;
    }
    off = addr & 0x1C;
    shift = (addr & 3u) * 8u;
    lane = lane_mask(size, shift);
    bits = ((uint32_t)value << shift) & lane;
    /* MASK filters PIN/SET/CLR only; DIR/MASK themselves are unmasked. */
    affect = (~s->mask[p]) & lane;

    switch (off) {
    case 0x00:
        s->dir[p] = (s->dir[p] & ~lane) | (bits & lane);
        break;
    case 0x10:
        s->mask[p] = (s->mask[p] & ~lane) | (bits & lane);
        break;
    case 0x14:
        s->out[p] = (s->out[p] & ~affect) | (bits & affect);
        break;
    case 0x18:
        s->out[p] |= bits & affect;
        break;
    case 0x1C:
        s->out[p] &= ~(bits & affect);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: bad offset 0x%" HWADDR_PRIx "\n",
                      __func__, addr);
        break;
    }
    if (off == 0x00 || off == 0x14 || off == 0x18 || off == 0x1C) {
        lpc1778_gpio_notify(s);
    }
}

void lpc1778_gpio_add_change_handler(Lpc1778GpioState *s,
                                     void (*fn)(void *), void *opaque)
{
    if (s->nchange >= LPC1778_GPIO_MAX_CHG) {
        qemu_log_mask(LOG_GUEST_ERROR, "%s: too many GPIO listeners\n",
                      __func__);
        return;
    }
    s->change[s->nchange] = fn;
    s->change_opaque[s->nchange] = opaque;
    s->nchange++;
}

static const MemoryRegionOps lpc1778_gpio_ops = {
    .read = lpc1778_gpio_read,
    .write = lpc1778_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void lpc1778_gpio_reset(DeviceState *dev)
{
    Lpc1778GpioState *s = LPC1778_GPIO(dev);
    int p;

    memset(s->dir, 0, sizeof(s->dir));
    memset(s->mask, 0, sizeof(s->mask));
    memset(s->out, 0, sizeof(s->out));
    /*
     * Datasheet 7.17.1 / 14.4: pads reset to inputs with the pull-up enabled,
     * so a pin nothing drives reads high. P0.7..P0.9 come up with no pull,
     * and P0.27..P0.31 / P5.2 / P5.3 have no pull-up either (I2C pads and
     * USB/analog pads), so those default low.
     */
    for (p = 0; p < LPC1778_GPIO_PORTS; p++) {
        s->in[p] = 0xFFFFFFFFu;
    }
    /*
     * P0.7: no pull → idle low. P0.8 (cover) is set in apply_board.
     * P0.9 FEED: firmware treats high as pressed; keep low when released.
     */
    s->in[0] &= ~(1u << 7);
    s->in[0] &= ~(0x1Fu << 27);
    s->in[5] &= ~(0x3u << 2);
    /* Boot strap: FEED must be released regardless of a prior QOM value. */
    s->feed_pressed = false;
    lpc1778_gpio_apply_board(s);
    /* Idle cutter/driver sensors until shtrih-mech takes over. */
    set_in_bit(s, 0, BIT_AC_SEN, true);
    set_in_bit(s, 0, BIT_NFAULT, true);
    set_in_bit(s, 0, BIT_FEED_KEY, false);
    lpc1778_gpio_notify(s);
}

static void lpc1778_gpio_get_pin0(Object *obj, Visitor *v, const char *name,
                                  void *opaque, Error **errp)
{
    uint32_t val = lpc1778_gpio_get_pins(LPC1778_GPIO(obj), 0);

    visit_type_uint32(v, name, &val, errp);
}

static void lpc1778_gpio_init(Object *obj)
{
    Lpc1778GpioState *s = LPC1778_GPIO(obj);

    s->paper_present = true;
    s->paper_near_end = true;
    s->cover_closed = true;
    s->feed_pressed = false;
    s->drawer_closed = true;

    object_property_add_bool(obj, "paper-present",
                             lpc1778_gpio_get_paper_present,
                             lpc1778_gpio_set_paper_present);
    object_property_add_bool(obj, "paper-near-end",
                             lpc1778_gpio_get_paper_near_end,
                             lpc1778_gpio_set_paper_near_end);
    object_property_add_bool(obj, "cover-closed",
                             lpc1778_gpio_get_cover_closed,
                             lpc1778_gpio_set_cover_closed);
    object_property_add_bool(obj, "feed-pressed",
                             lpc1778_gpio_get_feed_pressed,
                             lpc1778_gpio_set_feed_pressed);
    object_property_add_bool(obj, "drawer-closed",
                             lpc1778_gpio_get_drawer_closed,
                             lpc1778_gpio_set_drawer_closed);
    object_property_add_bool(obj, "led1", lpc1778_gpio_get_led1, NULL);
    object_property_add_bool(obj, "led2", lpc1778_gpio_get_led2, NULL);
    object_property_add(obj, "pin0", "uint32", lpc1778_gpio_get_pin0,
                        NULL, NULL, NULL);

    memory_region_init_io(&s->iomem, obj, &lpc1778_gpio_ops, s,
                          TYPE_LPC1778_GPIO, 0xC0);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_gpio_reset);
}

static const TypeInfo lpc1778_gpio_info = {
    .name = TYPE_LPC1778_GPIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778GpioState),
    .instance_init = lpc1778_gpio_init,
    .class_init = lpc1778_gpio_class_init,
};

static void lpc1778_gpio_register_types(void)
{
    type_register_static(&lpc1778_gpio_info);
}

type_init(lpc1778_gpio_register_types)
