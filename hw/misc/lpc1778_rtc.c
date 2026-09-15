/*
 * NXP LPC1778 RTC (UM10470) — battery-backed, independent of host wall clock.
 *
 * Time is stored in persist-path (next to the firmware image) and keeps
 * running from that epoch. Host clock is used only as a crystal (elapsed
 * seconds). First start without a file seeds once from the host.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/lpc1778_rtc.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define REG_ILR    0x00
#define REG_CTC    0x04
#define REG_CCR    0x08
#define REG_CIIR   0x0C
#define REG_AMR    0x10
#define REG_CTIME0 0x14
#define REG_CTIME1 0x18
#define REG_CTIME2 0x1C
#define REG_SEC    0x20
#define REG_MIN    0x24
#define REG_HOUR   0x28
#define REG_DOM    0x2C
#define REG_DOW    0x30
#define REG_DOY    0x34
#define REG_MONTH  0x38
#define REG_YEAR   0x3C
#define REG_CAL    0x40
#define REG_GPREG0 0x44
#define REG_AUXEN  0x58
#define REG_AUX    0x5C
#define REG_ALSEC  0x60

#define CCR_CLKEN  (1u << 0)
#define CCR_CTCRST (1u << 1)

#define RTC_FILE_MAGIC   0x31544352u /* 'RTC1' */
#define RTC_FILE_VERSION 1u

typedef struct Lpc1778RtcFile {
    uint32_t magic;
    uint32_t version;
    int64_t guest_sec;
    int64_t host_sec;
    uint32_t ccr;
    uint32_t cal;
    uint32_t gpreg[5];
} Lpc1778RtcFile;

static bool lpc1778_rtc_running(const Lpc1778RtcState *s)
{
    return (s->ccr & CCR_CLKEN) && !(s->ccr & CCR_CTCRST);
}

static int64_t lpc1778_rtc_host_now(void)
{
    return (int64_t)time(NULL);
}

static void lpc1778_rtc_update_irq(Lpc1778RtcState *s)
{
    qemu_set_irq(s->irq, s->ilr != 0);
}

static int64_t lpc1778_rtc_guest_now(const Lpc1778RtcState *s)
{
    if (lpc1778_rtc_running(s)) {
        int64_t dt = lpc1778_rtc_host_now() - s->host_sec;

        if (dt > 0) {
            return s->guest_sec + dt;
        }
    }
    return s->guest_sec;
}

static void lpc1778_rtc_latch(Lpc1778RtcState *s)
{
    s->guest_sec = lpc1778_rtc_guest_now(s);
    s->host_sec = lpc1778_rtc_host_now();
}

static void lpc1778_rtc_save(Lpc1778RtcState *s)
{
    Lpc1778RtcFile rec;
    g_autofree char *tmp = NULL;
    FILE *f;
    int i;

    if (!s->persist_path || !s->persist_path[0]) {
        return;
    }
    lpc1778_rtc_latch(s);
    rec.magic = RTC_FILE_MAGIC;
    rec.version = RTC_FILE_VERSION;
    rec.guest_sec = s->guest_sec;
    rec.host_sec = s->host_sec;
    rec.ccr = s->ccr;
    rec.cal = s->cal;
    for (i = 0; i < 5; i++) {
        rec.gpreg[i] = s->gpreg[i];
    }
    tmp = g_strdup_printf("%s.tmp", s->persist_path);
    f = fopen(tmp, "wb");
    if (!f) {
        return;
    }
    if (fwrite(&rec, sizeof(rec), 1, f) != 1) {
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

static bool lpc1778_rtc_load(Lpc1778RtcState *s)
{
    Lpc1778RtcFile rec;
    FILE *f;
    int i;

    if (!s->persist_path || !s->persist_path[0]) {
        return false;
    }
    f = fopen(s->persist_path, "rb");
    if (!f) {
        return false;
    }
    if (fread(&rec, sizeof(rec), 1, f) != 1) {
        fclose(f);
        return false;
    }
    fclose(f);
    if (rec.magic != RTC_FILE_MAGIC || rec.version != RTC_FILE_VERSION) {
        return false;
    }
    s->guest_sec = rec.guest_sec;
    s->host_sec = rec.host_sec;
    s->ccr = rec.ccr;
    s->cal = rec.cal;
    for (i = 0; i < 5; i++) {
        s->gpreg[i] = rec.gpreg[i];
    }
    /* Battery kept the oscillator running while QEMU was off. */
    lpc1778_rtc_latch(s);
    return true;
}

static void lpc1778_rtc_now(Lpc1778RtcState *s, struct tm *tm)
{
    time_t t = (time_t)lpc1778_rtc_guest_now(s);

    gmtime_r(&t, tm);
}

static void lpc1778_rtc_set_unix(Lpc1778RtcState *s, time_t t)
{
    s->guest_sec = (int64_t)t;
    s->host_sec = lpc1778_rtc_host_now();
    lpc1778_rtc_save(s);
}

static void lpc1778_rtc_rearm(Lpc1778RtcState *s)
{
    if (!s->tick) {
        return;
    }
    if (lpc1778_rtc_running(s)) {
        timer_mod(s->tick,
                  qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + NANOSECONDS_PER_SECOND);
    } else {
        timer_del(s->tick);
    }
}

static void lpc1778_rtc_tick(void *opaque)
{
    Lpc1778RtcState *s = opaque;

    if (lpc1778_rtc_running(s) && (s->ciir & 1u)) {
        s->ilr |= 1u;
        lpc1778_rtc_update_irq(s);
    }
    s->save_div++;
    if (s->save_div >= 60) {
        s->save_div = 0;
        lpc1778_rtc_save(s);
    }
    lpc1778_rtc_rearm(s);
}

static uint32_t lpc1778_rtc_ctime0(const struct tm *tm)
{
    return ((uint32_t)tm->tm_sec & 0x3f) |
           (((uint32_t)tm->tm_min & 0x3f) << 8) |
           (((uint32_t)tm->tm_hour & 0x1f) << 16) |
           (((uint32_t)tm->tm_wday & 0x7) << 24);
}

static uint32_t lpc1778_rtc_ctime1(const struct tm *tm)
{
    uint32_t year = (uint32_t)(tm->tm_year + 1900);

    return ((uint32_t)tm->tm_mday & 0x1f) |
           (((uint32_t)(tm->tm_mon + 1) & 0xf) << 8) |
           ((year & 0xfff) << 16);
}

static uint32_t lpc1778_rtc_ctime2(const struct tm *tm)
{
    return (uint32_t)(tm->tm_yday + 1) & 0xfff;
}

static uint64_t lpc1778_rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778RtcState *s = opaque;
    uint32_t off = addr & ~3u;
    struct tm tm;

    lpc1778_rtc_now(s, &tm);

    switch (off) {
    case REG_ILR:
        return s->ilr;
    case REG_CTC:
        return (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 30518) & 0xFFFE;
    case REG_CCR:
        return s->ccr;
    case REG_CIIR:
        return s->ciir;
    case REG_AMR:
        return s->amr;
    case REG_CTIME0:
        return lpc1778_rtc_ctime0(&tm);
    case REG_CTIME1:
        return lpc1778_rtc_ctime1(&tm);
    case REG_CTIME2:
        return lpc1778_rtc_ctime2(&tm);
    case REG_SEC:
        return tm.tm_sec & 0x3f;
    case REG_MIN:
        return tm.tm_min & 0x3f;
    case REG_HOUR:
        return tm.tm_hour & 0x1f;
    case REG_DOM:
        return tm.tm_mday & 0x1f;
    case REG_DOW:
        return tm.tm_wday & 0x7;
    case REG_DOY:
        return (tm.tm_yday + 1) & 0xfff;
    case REG_MONTH:
        return (tm.tm_mon + 1) & 0xf;
    case REG_YEAR:
        return (tm.tm_year + 1900) & 0xfff;
    case REG_CAL:
        return s->cal;
    case REG_GPREG0:
    case REG_GPREG0 + 4:
    case REG_GPREG0 + 8:
    case REG_GPREG0 + 12:
    case REG_GPREG0 + 16:
        return s->gpreg[(off - REG_GPREG0) / 4];
    case REG_AUXEN:
        return s->auxen;
    case REG_AUX:
        return s->aux;
    default:
        if (off >= REG_ALSEC && off <= REG_ALSEC + 0x1C) {
            return s->alarm[(off - REG_ALSEC) / 4];
        }
        return 0;
    }
}

static void lpc1778_rtc_write_time(Lpc1778RtcState *s, uint32_t off,
                                   uint32_t value)
{
    struct tm tm;
    time_t t;

    lpc1778_rtc_now(s, &tm);

    switch (off) {
    case REG_SEC:
        tm.tm_sec = value & 0x3f;
        break;
    case REG_MIN:
        tm.tm_min = value & 0x3f;
        break;
    case REG_HOUR:
        tm.tm_hour = value & 0x1f;
        break;
    case REG_DOM:
        tm.tm_mday = value & 0x1f;
        break;
    case REG_DOW:
        tm.tm_wday = value & 0x7;
        break;
    case REG_DOY:
        tm.tm_yday = (int)(value & 0xfff) - 1;
        break;
    case REG_MONTH:
        tm.tm_mon = (int)(value & 0xf) - 1;
        break;
    case REG_YEAR:
        tm.tm_year = (int)(value & 0xfff) - 1900;
        break;
    default:
        return;
    }
    tm.tm_isdst = 0;
    t = mktimegm(&tm);
    if (t != (time_t)-1) {
        lpc1778_rtc_set_unix(s, t);
    }
}

static void lpc1778_rtc_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778RtcState *s = opaque;
    uint32_t off = addr & ~3u;
    uint32_t v = (uint32_t)value;
    bool was_run = lpc1778_rtc_running(s);

    switch (off) {
    case REG_ILR:
        s->ilr &= ~v;
        lpc1778_rtc_update_irq(s);
        return;
    case REG_CCR:
        if (was_run && ((v & CCR_CLKEN) == 0 || (v & CCR_CTCRST))) {
            lpc1778_rtc_latch(s);
        }
        s->ccr = v;
        if (!was_run && lpc1778_rtc_running(s)) {
            s->host_sec = lpc1778_rtc_host_now();
        }
        lpc1778_rtc_save(s);
        lpc1778_rtc_rearm(s);
        return;
    case REG_CIIR:
        s->ciir = v & 0xff;
        lpc1778_rtc_rearm(s);
        return;
    case REG_AMR:
        s->amr = v & 0xff;
        return;
    case REG_CTIME0:
    case REG_CTIME1:
    case REG_CTIME2:
        return;
    case REG_SEC:
    case REG_MIN:
    case REG_HOUR:
    case REG_DOM:
    case REG_DOW:
    case REG_DOY:
    case REG_MONTH:
    case REG_YEAR:
        lpc1778_rtc_write_time(s, off, v);
        return;
    case REG_CAL:
        s->cal = v;
        lpc1778_rtc_save(s);
        return;
    case REG_GPREG0:
    case REG_GPREG0 + 4:
    case REG_GPREG0 + 8:
    case REG_GPREG0 + 12:
    case REG_GPREG0 + 16:
        s->gpreg[(off - REG_GPREG0) / 4] = v;
        lpc1778_rtc_save(s);
        return;
    case REG_AUXEN:
        s->auxen = v;
        return;
    case REG_AUX:
        s->aux &= ~v;
        return;
    default:
        if (off >= REG_ALSEC && off <= REG_ALSEC + 0x1C) {
            s->alarm[(off - REG_ALSEC) / 4] = v;
        }
        return;
    }
}

static const MemoryRegionOps lpc1778_rtc_ops = {
    .read = lpc1778_rtc_read,
    .write = lpc1778_rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_rtc_reset(DeviceState *dev)
{
    Lpc1778RtcState *s = LPC1778_RTC(dev);

    s->ilr = 0;
    lpc1778_rtc_update_irq(s);
}

static void lpc1778_rtc_realize(DeviceState *dev, Error **errp)
{
    Lpc1778RtcState *s = LPC1778_RTC(dev);

    s->aux = 0;
    if (!lpc1778_rtc_load(s)) {
        time_t now = time(NULL);
        struct tm loc;

        localtime_r(&now, &loc);
        loc.tm_isdst = 0;
        s->guest_sec = (int64_t)mktimegm(&loc);
        s->host_sec = lpc1778_rtc_host_now();
        s->ccr = CCR_CLKEN;
        lpc1778_rtc_save(s);
    }
    s->tick = timer_new_ns(QEMU_CLOCK_VIRTUAL, lpc1778_rtc_tick, s);
    lpc1778_rtc_rearm(s);
}

static void lpc1778_rtc_unrealize(DeviceState *dev)
{
    Lpc1778RtcState *s = LPC1778_RTC(dev);

    lpc1778_rtc_save(s);
    timer_free(s->tick);
    s->tick = NULL;
}

static void lpc1778_rtc_init(Object *obj)
{
    Lpc1778RtcState *s = LPC1778_RTC(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_rtc_ops, s,
                          TYPE_LPC1778_RTC, LPC1778_RTC_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const Property lpc1778_rtc_properties[] = {
    DEFINE_PROP_STRING("persist-path", Lpc1778RtcState, persist_path),
};

static void lpc1778_rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_rtc_reset);
    device_class_set_props(dc, lpc1778_rtc_properties);
    dc->realize = lpc1778_rtc_realize;
    dc->unrealize = lpc1778_rtc_unrealize;
}

static const TypeInfo lpc1778_rtc_info = {
    .name = TYPE_LPC1778_RTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778RtcState),
    .instance_init = lpc1778_rtc_init,
    .class_init = lpc1778_rtc_class_init,
};

static void lpc1778_rtc_register_types(void)
{
    type_register_static(&lpc1778_rtc_info);
}

type_init(lpc1778_rtc_register_types)
