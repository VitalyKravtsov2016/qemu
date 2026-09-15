/*
 * SHTRIH-M-01F print mechanism (SME16031.100.00).
 *
 *   BD63510 stepper   P2.0–5  (CW_CCW/ENABLE/MTH/CLK/MODE0/MODE1)
 *                     PWM1[4] on P2.3 is CLK. PS is not an MCU net here, so
 *                     it is taken as tied high: no standby, no 40 us wake and
 *                     no translator reset to the 45 deg initial angle.
 *   DRV8800 cutter    nSLEEP P0.0, nFAULT P0.1, PHASE P1.20,
 *                     MODE P1.21, ENABLE P1.23, AC_SEN P0.4
 *   Thermal head      nTLAT P0.11, TSCLK P0.15, TSTB0 P0.16, TSTB1 P0.17,
 *                     TSDATA P0.18, TSTB2..4 P5.0–2; 384 dots.
 *                     TSCLK/TSDATA are SCK0/MOSI0, so the dot data arrives
 *                     over SSP0 and the head sits on that bus. It has no chip
 *                     select: the shift register takes whatever SSP0 clocks
 *                     out (SME16031 has nothing else on SSP0).
 *   Cash drawer       P0.5 pulse after a cash receipt when table 1/1/6 ≠ 0.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/arm/shtrih_mech.h"
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/timer.h"

#define BIT_DRV_NSLEEP 0   /* P0 DRV8800 nSLEEP */
#define BIT_NFAULT     1   /* P0 DRV8800 nFAULT, open-drain */
#define BIT_AC_SEN     4   /* P0 home, high = idle */
#define BIT_TH_LAT     11  /* P0 nTLAT, active-low */
#define BIT_TH_CLK     15
#define BIT_TH_STB0    16
#define BIT_TH_STB1    17
#define BIT_TH_DATA    18
#define BIT_DRAWER_KICK 5  /* P0 solenoid; table 1/1/6, pulse after cash close */

#define BIT_AC_PHASE   20  /* P1 DRV8800 PHASE */
#define BIT_AC_MODE    21  /* P1 DRV8800 MODE (slow/fast decay) */
#define BIT_AC_EN      23  /* P1 DRV8800 ENABLE */
#define BIT_LED1       27  /* P1 */
#define BIT_LED2       28  /* P1 */

#define BIT_STEP_DIR   0   /* P2 BD63510 CW_CCW, L = CW */
#define BIT_STEP_EN    1   /* P2 BD63510 ENABLE */
#define BIT_STEP_MTH   2   /* P2 STEP_MODE_CURRENT → MTH decay select */
#define BIT_STEP_CLK   3   /* P2 BD63510 CLK */
#define BIT_STEP_MODE0 4   /* P2 BD63510 MODE_0, excitation LSB */
#define BIT_STEP_MODE1 5   /* P2 BD63510 MODE_1 */

#define BIT_TH_STB2    0   /* P5 */
#define BIT_TH_STB3    1
#define BIT_TH_STB4    2

#define PWM_STEP_CH    4   /* PWM1[4] = STEP_CLK */
#define BIT_TPE_OPT    12  /* P0 TPE, active-low paper in the slot */
#define CUT_LEAVE_MS   12  /* stay home after AC_EN so firmware can sample it */
#define CUT_RETURN_MS  40  /* time to reach home after reverse / ENABLE off */
#define CUT_COAST_MS   5   /* ignore ENABLE PWM valleys this long */
#define CUT_GAP_MS     20  /* after AC_SEN home, then pulse TPE (eject poll) */
#define STEP_UNIT_FULL 16  /* paper_pos granularity is one 1/16 step */
/*
 * After 17h the firmware only line-feeds (font cell + inter-line gap).
 * Extra CLK after that is not a real tape advance for the receipt window.
 */
#define TEXT_LINE_ROWS       24
#define RECEIPT_TRAIL_ROWS   TEXT_LINE_ROWS
#define POLL_GAP_MS    250
#define DRV_WAKE_NS    1000000  /* DRV8800 8.3.6: 1 ms charge-pump settle */

#define CUT_IDLE       0
#define CUT_HOLD_HOME  1
#define CUT_AWAY       2
#define CUT_RETURNING  3
#define CUT_COAST      4

static void shtrih_receipt_advance(ShtrihMechState *s);
static void shtrih_paper_gap_begin(ShtrihMechState *s);
static void shtrih_paper_gap_end(ShtrihMechState *s);

static bool pin_high(uint32_t v, unsigned bit)
{
    return (v & (1u << bit)) != 0;
}

static bool rose(uint32_t old, uint32_t now, unsigned bit)
{
    uint32_t m = 1u << bit;

    return (now & m) != 0 && (old & m) == 0;
}

static bool fell(uint32_t old, uint32_t now, unsigned bit)
{
    uint32_t m = 1u << bit;

    return (old & m) != 0 && (now & m) == 0;
}

static void shtrih_mech_set_nfault(ShtrihMechState *s, bool ok)
{
    lpc1778_gpio_set_in_bit(s->gpio, 0, BIT_NFAULT, ok);
}

static void shtrih_mech_set_home(ShtrihMechState *s, bool home)
{
    s->cutter_away = !home;
    lpc1778_gpio_set_in_bit(s->gpio, 0, BIT_AC_SEN, home);
}

/* DRV8800 Table 8-1: H-bridge drives only if nSLEEP=1 and ENABLE=1. */
static bool drv8800_drive(uint32_t p0, uint32_t p1)
{
    return pin_high(p0, BIT_DRV_NSLEEP) && pin_high(p1, BIT_AC_EN);
}

/*
 * BD63510 excitation mode: MODE_0 is the LSB.
 *   L L = full, H L = half, L H = quarter, H H = 1/16
 * Returned in 1/16-step units so paper travel per CLK is exact.
 */
static int32_t bd63510_units_per_clk(uint32_t p2)
{
    unsigned mode = (pin_high(p2, BIT_STEP_MODE0) ? 1u : 0u) |
                    (pin_high(p2, BIT_STEP_MODE1) ? 2u : 0u);

    switch (mode) {
    case 0:
        return STEP_UNIT_FULL;
    case 1:
        return STEP_UNIT_FULL / 2;
    case 2:
        return STEP_UNIT_FULL / 4;
    default:
        return 1;
    }
}

static void shtrih_mech_step(ShtrihMechState *s)
{
    uint32_t p2;
    int32_t units;

    /* BD63510 ENABLE=L: CLK blocked, outputs OPEN, electrical angle held. */
    if (!s->gpio) {
        return;
    }
    p2 = s->gpio->out[2];
    if (!pin_high(p2, BIT_STEP_EN)) {
        return;
    }
    /* CW_CCW is sampled by this same rising edge; L = CW = paper forward. */
    units = bd63510_units_per_clk(p2);
    s->paper_pos += pin_high(p2, BIT_STEP_DIR) ? -units : units;
    s->step_count++;
    /* Paper in the slot moves: a full step is one blank (or already inked) row. */
    shtrih_receipt_advance(s);
}

static void shtrih_paper_gap_end(ShtrihMechState *s)
{
    s->paper_gap_on = false;
    s->paper_gap_armed = false;
    s->paper_gap_deadline_ms = 0;
    if (s->paper_gap_timer) {
        timer_del(s->paper_gap_timer);
    }
    if (!s->gpio) {
        return;
    }
    s->gpio->paper_present = s->paper_was;
    lpc1778_gpio_apply_board(s->gpio);
    lpc1778_gpio_set_in_bit(s->gpio, 0, BIT_TPE_OPT, !s->paper_was);
}

static void shtrih_paper_gap_arm(ShtrihMechState *s, int64_t delay_ms)
{
    s->paper_gap_deadline_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + delay_ms;
    if (s->paper_gap_timer) {
        timer_mod(s->paper_gap_timer, s->paper_gap_deadline_ms);
    }
}

static void shtrih_paper_gap_poll(ShtrihMechState *s)
{
    int64_t now;

    if (!s->paper_gap_armed && !s->paper_gap_on) {
        return;
    }
    now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    if (now < s->paper_gap_deadline_ms) {
        return;
    }
    if (s->paper_gap_armed) {
        s->paper_gap_armed = false;
        shtrih_paper_gap_begin(s);
        return;
    }
    if (s->paper_gap_on) {
        shtrih_paper_gap_end(s);
    }
}

static void shtrih_paper_gap_tick(void *opaque)
{
    shtrih_paper_gap_poll(opaque);
}

static void shtrih_paper_gap_begin(ShtrihMechState *s)
{
    if (!s->gpio || s->paper_gap_on || !s->lines_fired) {
        return;
    }
    if (!s->gpio->paper_present) {
        return;
    }
    /*
     * REALTIME deadline on a REALTIME timer. A VIRTUAL stamp here is ~boot
     * ms while the clock is wall time, so the tick never matches — TPE stays
     * empty until the UI toggles paper, and firmware latches ERR 71h.
     */
    s->paper_was = s->gpio->paper_present;
    s->paper_gap_on = true;
    s->gpio->paper_present = false;
    lpc1778_gpio_apply_board(s->gpio);
    lpc1778_gpio_set_in_bit(s->gpio, 0, BIT_TPE_OPT, true);
    /* Also expire from GPIO activity: icount can starve the REALTIME timer. */
    shtrih_paper_gap_arm(s, POLL_GAP_MS);
}

static void shtrih_mech_schedule_return(ShtrihMechState *s)
{
    s->cut_phase = CUT_RETURNING;
    if (s->cut_timer) {
        timer_mod(s->cut_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + CUT_RETURN_MS);
    }
}

static void shtrih_mech_cut_tick(void *opaque)
{
    ShtrihMechState *s = opaque;

    if (s->cut_phase == CUT_HOLD_HOME) {
        s->cut_phase = CUT_AWAY;
        shtrih_mech_set_home(s, false);
        /*
         * Stay away while ENABLE+PHASE keep the H-bridge in the leave
         * direction. Home on PHASE reverse or ENABLE off (see gpio_change).
         */
        return;
    }
    if (s->cut_phase == CUT_AWAY) {
        shtrih_mech_schedule_return(s);
        return;
    }
    if (s->cut_phase == CUT_COAST) {
        if (s->gpio && drv8800_drive(s->gpio->out[0], s->gpio->out[1])) {
            s->cut_phase = CUT_AWAY;
            return;
        }
        shtrih_mech_schedule_return(s);
        return;
    }
    if (s->cut_phase == CUT_RETURNING) {
        /* Spring return (ENABLE off) or powered reverse reached home. */
        s->cut_phase = CUT_IDLE;
        shtrih_mech_set_home(s, true);
        /*
         * Ticket has left the knife. Delay the TPE pulse so the cut FSM
         * can sample AC_SEN home with paper still present; then the eject
         * poller sees the gap. Same-instant empty TPE → ERR 71h.
         */
        s->paper_gap_armed = true;
        shtrih_paper_gap_arm(s, CUT_GAP_MS);
    }
}

static void shtrih_mech_cut(ShtrihMechState *s);

/* Charge pump has settled: honour a drive request issued during the delay. */
static void shtrih_mech_drv_tick(void *opaque)
{
    ShtrihMechState *s = opaque;

    if (!s->gpio || !drv8800_drive(s->gpio->out[0], s->gpio->out[1])) {
        return;
    }
    shtrih_mech_cut(s);
}

static void shtrih_mech_cut(ShtrihMechState *s)
{
    /*
     * ENABLE is often PWM-chopped while the blade is off-home. Re-asserting
     * the H-bridge must not teleport the cam back to the home opto — that
     * chatters AC_SEN and the print FSM latches ERR 71h.
     */
    if (s->cut_phase == CUT_HOLD_HOME) {
        return;
    }
    if (s->cut_phase == CUT_COAST) {
        s->cut_phase = CUT_AWAY;
        if (s->cut_timer) {
            timer_del(s->cut_timer);
        }
        return;
    }
    if (s->cut_phase == CUT_AWAY || s->cut_phase == CUT_RETURNING) {
        /* PWM chop: H-bridge current pulses, the cam keeps moving. */
        return;
    }
    s->cut_count++;
    if (s->paper_gap_on) {
        shtrih_paper_gap_end(s);
    }
    s->paper_gap_armed = false;
    if (s->paper_gap_timer) {
        timer_del(s->paper_gap_timer);
    }
    /*
     * AC_SEN is the home opto, not a DRV8800 pin. Firmware samples
     * P0.4 high twice, then low twice. Leave home only after the motor
     * has been driven (ENABLE and nSLEEP) for CUT_LEAVE_MS.
     */
    s->cut_phase = CUT_HOLD_HOME;
    shtrih_mech_set_home(s, true);
    if (s->cut_timer) {
        timer_mod(s->cut_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + CUT_LEAVE_MS);
    }
}

static void shtrih_thermal_shift(ShtrihMechState *s, bool data_high)
{
    int byte_i, bit_i;

    if (s->bit_pos >= SHTRIH_HEAD_MAX_BITS) {
        s->bit_pos = 0;
    }
    byte_i = s->bit_pos >> 3;
    bit_i = 7 - (s->bit_pos & 7);
    if (data_high) {
        s->shift[byte_i] |= (uint8_t)(1u << bit_i);
    } else {
        s->shift[byte_i] &= (uint8_t)~(1u << bit_i);
    }
    s->bit_pos++;
    s->head_bits++;
}

static uint32_t shtrih_thermal_transfer(SSIPeripheral *dev, uint32_t val)
{
    ShtrihMechState *s = SHTRIH_MECH(dev);
    /* One clock per bit, so the SSP word length is what the head shifts in. */
    int bits = s->ssp ? (int)lpc1778_ssp_word_bits(s->ssp) : 8;
    int i;

    /* MSB first out of MOSI0, so the first dot clocked in is the top bit. */
    for (i = bits - 1; i >= 0; i--) {
        shtrih_thermal_shift(s, ((val >> i) & 1u) != 0);
    }
    s->head_words++;
    /* Nothing drives MISO0 back: the head is write-only. */
    return 0;
}

static bool shtrih_row_nonzero(const uint8_t *row)
{
    int i;

    for (i = 0; i < SHTRIH_RECEIPT_BYTES; i++) {
        if (row[i]) {
            return true;
        }
    }
    return false;
}

static bool shtrih_bit_at(const uint8_t *src, int bit)
{
    return (src[bit >> 3] & (uint8_t)(0x80 >> (bit & 7))) != 0;
}

/*
 * Copy the shift-register line into the receipt row. Do not slide by ink:
 * that jittered double-width glyphs. Keep up to SHTRIH_RECEIPT_DOTS so text
 * past the 384-heater mark still appears in the tape view.
 */
static void shtrih_pack_dots(const ShtrihMechState *s, uint8_t *dst)
{
    int nbits = s->line_bits;
    int i, limit;

    memset(dst, 0, SHTRIH_RECEIPT_BYTES);
    if (nbits > SHTRIH_HEAD_MAX_BITS) {
        nbits = SHTRIH_HEAD_MAX_BITS;
    }
    limit = nbits;
    if (limit > SHTRIH_RECEIPT_DOTS) {
        limit = SHTRIH_RECEIPT_DOTS;
    }
    for (i = 0; i < limit; i++) {
        if (shtrih_bit_at(s->latched, i)) {
            dst[i >> 3] |= (uint8_t)(0x80 >> (i & 7));
        }
    }
}

static void shtrih_receipt_append(ShtrihMechState *s, const uint8_t *row)
{
    if (!s->receipt || s->receipt_n >= SHTRIH_RECEIPT_MAX_ROWS) {
        return;
    }
    memcpy(s->receipt + s->receipt_n * SHTRIH_RECEIPT_BYTES,
           row, SHTRIH_RECEIPT_BYTES);
    s->receipt_n++;
}

static void shtrih_receipt_ensure_up_to(ShtrihMechState *s, int32_t row)
{
    uint8_t blank[SHTRIH_RECEIPT_BYTES];
    int32_t next;

    if (!s->receipt || row < 0) {
        return;
    }
    memset(blank, 0, sizeof(blank));
    if (!s->have_paint_row || s->receipt_n == 0) {
        shtrih_receipt_append(s, blank);
        s->last_paint_row = row;
        s->have_paint_row = true;
        return;
    }
    if (row <= s->last_paint_row) {
        return;
    }
    /*
     * A printed line is followed only by the inter-line feed. Do not keep
     * laying down blank tape if the stepper is still clocking after that.
     */
    if (s->have_ink_row && row > s->last_ink_row + RECEIPT_TRAIL_ROWS) {
        row = s->last_ink_row + RECEIPT_TRAIL_ROWS;
        if (row <= s->last_paint_row) {
            return;
        }
    }
    next = s->last_paint_row + 1;
    while (next <= row && s->receipt_n < SHTRIH_RECEIPT_MAX_ROWS) {
        shtrih_receipt_append(s, blank);
        s->last_paint_row = next;
        next++;
    }
}

/*
 * One visible tape row per full step of the BD63510. Empty feed is still
 * paper: it must show as blank lines, same as a real roll.
 */
static void shtrih_receipt_advance(ShtrihMechState *s)
{
    int32_t row = s->paper_pos / STEP_UNIT_FULL;

    /*
     * Idle stepper CLK before any burn must not grow the tape view — otherwise
     * the receipt crawls while the motor bit-bangs blank lines after boot.
     * Inter-line blank after ink is handled in ensure_up_to (RECEIPT_TRAIL_ROWS).
     */
    if (!s->have_ink_row) {
        return;
    }
    if (!s->have_paint_row && row <= 0) {
        return;
    }
    shtrih_receipt_ensure_up_to(s, row);
}

static uint8_t *shtrih_receipt_line(ShtrihMechState *s, int32_t row)
{
    int32_t back;

    if (!s->have_paint_row || !s->receipt_n) {
        return NULL;
    }
    back = s->last_paint_row - row;
    if (back < 0 || (uint32_t)back >= s->receipt_n) {
        return NULL;
    }
    return s->receipt + (s->receipt_n - 1 - (uint32_t)back) * SHTRIH_RECEIPT_BYTES;
}

/*
 * A burned line sits at the current stepper position. Strobe banks on the
 * same paper row are OR-merged. Blank latches do not add extra rows: the
 * stepper already laid down the paper.
 */
static void shtrih_receipt_paint(ShtrihMechState *s)
{
    uint8_t packed[SHTRIH_RECEIPT_BYTES];
    uint8_t *dst;
    int32_t row;
    int b;

    if (!s->latched_valid) {
        return;
    }
    row = s->paper_pos / STEP_UNIT_FULL;
    if (row < 0) {
        row = 0;
    }
    shtrih_receipt_ensure_up_to(s, row);
    shtrih_pack_dots(s, packed);
    if (!shtrih_row_nonzero(packed)) {
        return;
    }
    dst = shtrih_receipt_line(s, row);
    if (!dst) {
        shtrih_receipt_append(s, packed);
        s->last_paint_row = row;
        s->have_paint_row = true;
    } else {
        for (b = 0; b < SHTRIH_RECEIPT_BYTES; b++) {
            dst[b] |= packed[b];
        }
    }
    s->lines_fired++;
    s->last_ink_row = row;
    s->have_ink_row = true;
}

static void shtrih_thermal_fire(ShtrihMechState *s)
{
    if (!s->latched_valid || !s->line_pending) {
        return;
    }
    s->line_pending = false;
    shtrih_receipt_paint(s);
}

static void shtrih_thermal_latch(ShtrihMechState *s)
{
    if (s->bit_pos == 0) {
        return;
    }
    memcpy(s->latched, s->shift, sizeof(s->latched));
    s->line_bits = s->bit_pos;
    s->latched_valid = true;
    s->line_pending = true;
    s->latch_count++;
    memset(s->shift, 0, sizeof(s->shift));
    s->bit_pos = 0;
    /* STB may be PWM-driven and invisible as GPIO — publish on latch. */
    shtrih_thermal_fire(s);
}

static void shtrih_mech_gpio_change(void *opaque)
{
    ShtrihMechState *s = opaque;
    uint32_t now[LPC1778_GPIO_PORTS];
    uint32_t old[LPC1778_GPIO_PORTS];
    int p;

    if (!s->gpio) {
        return;
    }
    shtrih_paper_gap_poll(s);
    for (p = 0; p < LPC1778_GPIO_PORTS; p++) {
        now[p] = s->gpio->out[p];
    }
    memcpy(old, s->last_out, sizeof(old));
    memcpy(s->last_out, now, sizeof(s->last_out));

    /*
     * nFAULT is open-drain and not valid while nSLEEP=0. No OCP/UVLO/OTS
     * in this model; board pull-up keeps the pin high in sleep and awake.
     */
    if ((now[0] ^ old[0]) & (1u << BIT_DRV_NSLEEP)) {
        shtrih_mech_set_nfault(s, true);
        if (rose(old[0], now[0], BIT_DRV_NSLEEP)) {
            s->drv_wake_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) +
                             DRV_WAKE_NS;
        }
    }

    /*
     * Cutter motion follows the H-bridge, not ENABLE alone. nSLEEP=0 is
     * Hi-Z (Table 8-1). ENABLE=0 is brake (MODE=1) or fast-decay coast.
     *
     * Real sequence: PHASE forward + ENABLE → leave home; PHASE reverse
     * (ENABLE still on) → return home; then ENABLE off. Older paths drop
     * ENABLE after the low and rely on the cam spring. Model both.
     *
     * Analog travel and the DRV8800 charge pump are wall-clock: VIRTUAL
     * deadlines under -icount do not elapse while the guest busy-waits
     * on GPIO, so AC_SEN never moves and firmware latches ERR 71h.
     */
    {
        bool drive = drv8800_drive(now[0], now[1]);
        bool was = drv8800_drive(old[0], old[1]);
        int64_t t = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        bool phase_edge = ((now[1] ^ old[1]) & (1u << BIT_AC_PHASE)) != 0;

        if (drive && !was) {
            if (t < s->drv_wake_ns && s->drv_timer) {
                /* Outputs cannot drive until the charge pump is up. */
                timer_mod_ns(s->drv_timer, s->drv_wake_ns);
            } else {
                shtrih_mech_cut(s);
            }
        }
        if (drive && was && phase_edge) {
            /*
             * Direction flip under power: after leaving home, reverse brings
             * the blade back. Without this AC_SEN stays low → ERR 71h.
             */
            if (s->cut_phase == CUT_AWAY || s->cutter_away) {
                shtrih_mech_schedule_return(s);
            }
        }
        if (!drive && phase_edge) {
            if (s->cut_phase == CUT_AWAY || s->cutter_away) {
                shtrih_mech_schedule_return(s);
            }
        }
        if (!drive && was) {
            if (s->drv_timer) {
                timer_del(s->drv_timer);
            }
            /*
             * Firmware polls AC_SEN while ENABLE is on, then drops ENABLE.
             * A pulse shorter than CUT_LEAVE_MS still has to produce a low
             * or the print FSM never leaves the knife wait.
             */
            if (s->cut_phase == CUT_HOLD_HOME) {
                shtrih_mech_set_home(s, false);
                s->cut_phase = CUT_COAST;
                if (s->cut_timer) {
                    timer_mod(s->cut_timer,
                              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                              CUT_COAST_MS);
                }
            } else if (s->cut_phase == CUT_AWAY) {
                s->cut_phase = CUT_COAST;
                if (s->cut_timer) {
                    timer_mod(s->cut_timer,
                              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) +
                              CUT_COAST_MS);
                }
            } else if (s->cut_phase == CUT_RETURNING) {
                /* Already coming home (PHASE reverse); keep that timer. */
            }
        }
    }

    /*
     * Latch rising edges on GPIO outputs that are not the cutter / stepper /
     * thermal / LED nets. Print activity always hits the same spare pads;
     * a cash-drawer solenoid adds a bit that only appears after a cash close.
     */
    {
        static const uint32_t mech_out_mask[LPC1778_GPIO_PORTS] = {
            [0] = (1u << BIT_DRV_NSLEEP) | (1u << BIT_TH_LAT) |
                  (1u << BIT_TH_CLK) | (1u << BIT_TH_STB0) |
                  (1u << BIT_TH_STB1) | (1u << BIT_TH_DATA),
            [1] = (1u << BIT_AC_PHASE) | (1u << BIT_AC_MODE) |
                  (1u << BIT_AC_EN) | (1u << BIT_LED1) | (1u << BIT_LED2),
            [2] = (1u << BIT_STEP_DIR) | (1u << BIT_STEP_EN) |
                  (1u << BIT_STEP_MTH) | (1u << BIT_STEP_CLK) |
                  (1u << BIT_STEP_MODE0) | (1u << BIT_STEP_MODE1),
            [5] = (1u << BIT_TH_STB2) | (1u << BIT_TH_STB3) |
                  (1u << BIT_TH_STB4),
        };

        for (p = 0; p < LPC1778_GPIO_PORTS; p++) {
            /* Latch any unused pad edge, including DIR input→output while
             * the out latch is already high (common solenoid kick). */
            uint32_t dir_now = s->gpio->dir[p];
            uint32_t dir_old = s->last_dir[p];
            uint32_t out_edge = (old[p] ^ now[p]) & dir_now & ~mech_out_mask[p];
            uint32_t dir_on = (~dir_old & dir_now) & now[p] & ~mech_out_mask[p];
            uint32_t dir_off = (dir_old & ~dir_now) & old[p] & ~mech_out_mask[p];
            uint32_t hit = out_edge | dir_on | dir_off;

            s->last_dir[p] = dir_now;
            s->gpio_rise[p] |= hit;
        }
        if ((s->gpio->dir[0] & (1u << BIT_DRAWER_KICK)) &&
            ((old[0] ^ now[0]) & (1u << BIT_DRAWER_KICK))) {
            int64_t t = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            bool high = pin_high(now[0], BIT_DRAWER_KICK);

            if (s->drawer_edge_ns && s->drawer_pin_high != high) {
                uint32_t ms = (uint32_t)((t - s->drawer_edge_ns) / 1000000);
                if (s->drawer_pin_high && s->drawer_wn < 8) {
                    s->drawer_w_ms[s->drawer_wn++] = ms;
                } else if (!s->drawer_pin_high && s->drawer_gn < 8) {
                    s->drawer_g_ms[s->drawer_gn++] = ms;
                }
            }
            s->drawer_edge_ns = t;
            s->drawer_pin_high = high;
            if (high) {
                s->drawer_kick_count++;
                s->drawer_kick_pin = BIT_DRAWER_KICK;
            }
        }
    }

    /* BD63510: electrical angle advances on CLK rising edge only. */
    if (rose(old[2], now[2], BIT_STEP_CLK)) {
        shtrih_mech_step(s);
    }

    /* Thermal: DATA sampled on TSCLK rise; latch on nTLAT fall. */
    if (now[0] != old[0]) {
        if (rose(old[0], now[0], BIT_TH_CLK)) {
            shtrih_thermal_shift(s, pin_high(now[0], BIT_TH_DATA));
        }
        if (fell(old[0], now[0], BIT_TH_LAT)) {
            shtrih_thermal_latch(s);
        }
        /*
         * DD8 (74HCT244) is non-inverting, so a low MCU pin holds nTSTB_x
         * asserted and the dots heat. The rising edge ends the burn pulse:
         * that is when the line is on the paper.
         */
        if (rose(old[0], now[0], BIT_TH_STB0) ||
            rose(old[0], now[0], BIT_TH_STB1)) {
            s->strobe_count++;
            shtrih_thermal_fire(s);
        }
    }
    if (rose(old[5], now[5], BIT_TH_STB2) ||
        rose(old[5], now[5], BIT_TH_STB3) ||
        rose(old[5], now[5], BIT_TH_STB4)) {
        s->strobe_count++;
        shtrih_thermal_fire(s);
    }
}

static void shtrih_mech_pwm_match(void *opaque, int channel)
{
    ShtrihMechState *s = opaque;

    /*
     * Single-edge PWM1[4]: high at period reset (channel 0), low at MR4.
     * BD63510 CLK = rising edge → period start, not the MR4 match.
     */
    if (channel != 0 || !s->pwm) {
        return;
    }
    if (!(s->pwm->pcr & (1u << (8 + PWM_STEP_CH)))) {
        return;
    }
    /*
     * UM10470 26.4.1: match 0 means constant low, and a match at or beyond
     * MR0 leaves the output stuck high (clear wins on a tie). Either way the
     * line stops pulsing, so the stepper sees no clock.
     */
    if (s->pwm->mr[PWM_STEP_CH] == 0 ||
        s->pwm->mr[PWM_STEP_CH] >= s->pwm->mr[0]) {
        return;
    }
    shtrih_mech_step(s);
}

static void shtrih_mech_get_u32(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    uint32_t *field = opaque;

    visit_type_uint32(v, name, field, errp);
}

static void shtrih_mech_get_paper_pos(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    ShtrihMechState *s = SHTRIH_MECH(obj);
    int64_t pos = s->paper_pos;

    (void)opaque;
    visit_type_int(v, name, &pos, errp);
}

static void shtrih_mech_get_home(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    ShtrihMechState *s = SHTRIH_MECH(obj);
    bool home = !s->cutter_away;

    (void)opaque;
    visit_type_bool(v, name, &home, errp);
}

static void shtrih_mech_get_step_en(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    ShtrihMechState *s = SHTRIH_MECH(obj);
    bool en = s->gpio && pin_high(s->gpio->out[2], BIT_STEP_EN);

    (void)opaque;
    visit_type_bool(v, name, &en, errp);
}

static void shtrih_mech_get_step_dir(Object *obj, Visitor *v, const char *name,
                                     void *opaque, Error **errp)
{
    ShtrihMechState *s = SHTRIH_MECH(obj);
    bool dir = s->gpio && pin_high(s->gpio->out[2], BIT_STEP_DIR);

    (void)opaque;
    visit_type_bool(v, name, &dir, errp);
}

static void shtrih_mech_get_cut_dir(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    ShtrihMechState *s = SHTRIH_MECH(obj);
    bool dir = s->gpio && pin_high(s->gpio->out[1], BIT_AC_PHASE);

    (void)opaque;
    visit_type_bool(v, name, &dir, errp);
}

static void shtrih_mech_get_row(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
    ShtrihMechState *s = SHTRIH_MECH(obj);
    char hex[SHTRIH_RECEIPT_BYTES * 2 + 1];
    uint8_t packed[SHTRIH_RECEIPT_BYTES];
    char *str;
    int i;

    (void)opaque;
    shtrih_pack_dots(s, packed);
    for (i = 0; i < SHTRIH_RECEIPT_BYTES; i++) {
        snprintf(hex + i * 2, 3, "%02X", packed[i]);
    }
    str = g_strdup(hex);
    visit_type_str(v, name, &str, errp);
    g_free(str);
}

/* Whole latched line as it arrived, however long it was. */
static void shtrih_mech_get_line(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    ShtrihMechState *s = SHTRIH_MECH(obj);
    int bytes = (s->line_bits + 7) / 8;
    GString *gs = g_string_new(NULL);
    char *str;
    int i;

    (void)opaque;
    for (i = 0; i < bytes && i < SHTRIH_HEAD_MAX_BYTES; i++) {
        g_string_append_printf(gs, "%02X", s->latched[i]);
    }
    str = g_string_free(gs, FALSE);
    visit_type_str(v, name, &str, errp);
    g_free(str);
}

static void shtrih_mech_get_line_bits(Object *obj, Visitor *v, const char *name,
                                      void *opaque, Error **errp)
{
    ShtrihMechState *s = SHTRIH_MECH(obj);
    int64_t bits = s->line_bits;

    (void)opaque;
    visit_type_int(v, name, &bits, errp);
}

static void shtrih_mech_get_pull(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    ShtrihMechState *s = SHTRIH_MECH(obj);
    GString *gs;
    char *str;
    uint32_t i, j;

    (void)opaque;
    (void)name;
    gs = g_string_new(NULL);
    for (i = s->receipt_rd; i < s->receipt_n; i++) {
        const uint8_t *row = s->receipt + i * SHTRIH_RECEIPT_BYTES;

        for (j = 0; j < SHTRIH_RECEIPT_BYTES; j++) {
            g_string_append_printf(gs, "%02X", row[j]);
        }
        g_string_append_c(gs, '\n');
    }
    /* Everything handed over: start the buffer again so long runs keep going. */
    s->receipt_rd = 0;
    s->receipt_n = 0;
    str = g_string_free(gs, FALSE);
    visit_type_str(v, name, &str, errp);
    g_free(str);
}

static void shtrih_mech_get_rise(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    ShtrihMechState *s = SHTRIH_MECH(obj);
    char buf[96];
    char *str;

    (void)opaque;
    snprintf(buf, sizeof(buf),
             "P0=%08X P1=%08X P2=%08X P3=%08X P4=%08X P5=%08X",
             s->gpio_rise[0], s->gpio_rise[1], s->gpio_rise[2],
             s->gpio_rise[3], s->gpio_rise[4], s->gpio_rise[5]);
    memset(s->gpio_rise, 0, sizeof(s->gpio_rise));
    str = g_strdup(buf);
    visit_type_str(v, name, &str, errp);
    g_free(str);
}

static void shtrih_mech_get_kick_trace(Object *obj, Visitor *v, const char *name,
                                       void *opaque, Error **errp)
{
    ShtrihMechState *s = SHTRIH_MECH(obj);
    GString *gs;
    char *str;
    uint32_t i;

    (void)opaque;
    (void)name;
    gs = g_string_new(NULL);
    g_string_append_printf(gs, "n=%u w=", s->drawer_kick_count);
    for (i = 0; i < s->drawer_wn; i++) {
        g_string_append_printf(gs, "%s%u", i ? "," : "", s->drawer_w_ms[i]);
    }
    g_string_append(gs, " g=");
    for (i = 0; i < s->drawer_gn; i++) {
        g_string_append_printf(gs, "%s%u", i ? "," : "", s->drawer_g_ms[i]);
    }
    s->drawer_kick_count = 0;
    s->drawer_wn = 0;
    s->drawer_gn = 0;
    s->drawer_edge_ns = 0;
    s->drawer_pin_high = false;
    str = g_string_free(gs, FALSE);
    visit_type_str(v, name, &str, errp);
    g_free(str);
}

static void shtrih_mech_reset(DeviceState *dev)
{
    ShtrihMechState *s = SHTRIH_MECH(dev);

    memset(s->last_out, 0, sizeof(s->last_out));
    memset(s->last_dir, 0, sizeof(s->last_dir));
    s->step_count = 0;
    s->paper_pos = 0;
    s->drv_wake_ns = 0;
    s->cut_count = 0;
    s->drawer_kick_count = 0;
    s->drawer_kick_pin = 0;
    s->drawer_edge_ns = 0;
    s->drawer_pin_high = false;
    s->drawer_wn = 0;
    s->drawer_gn = 0;
    memset(s->gpio_rise, 0, sizeof(s->gpio_rise));
    s->lines_fired = 0;
    s->receipt_n = 0;
    s->receipt_rd = 0;
    s->last_paint_row = 0;
    s->have_paint_row = false;
    s->last_ink_row = 0;
    s->have_ink_row = false;
    s->bit_pos = 0;
    s->line_bits = 0;
    s->latched_valid = false;
    s->line_pending = false;
    s->head_bits = 0;
    s->head_words = 0;
    s->latch_count = 0;
    s->strobe_count = 0;
    memset(s->shift, 0, sizeof(s->shift));
    memset(s->latched, 0, sizeof(s->latched));
    s->cut_phase = CUT_IDLE;
    if (s->cut_timer) {
        timer_del(s->cut_timer);
    }
    if (s->drv_timer) {
        timer_del(s->drv_timer);
    }
    if (s->paper_gap_timer) {
        timer_del(s->paper_gap_timer);
    }
    if (s->paper_gap_on && s->gpio) {
        s->gpio->paper_present = s->paper_was;
        lpc1778_gpio_apply_board(s->gpio);
    }
    s->paper_gap_on = false;
    s->paper_gap_armed = false;
    s->paper_gap_deadline_ms = 0;
    s->paper_was = true;
    if (s->gpio) {
        memcpy(s->last_out, s->gpio->out, sizeof(s->last_out));
        memcpy(s->last_dir, s->gpio->dir, sizeof(s->last_dir));
        shtrih_mech_set_nfault(s, true);
        shtrih_mech_set_home(s, true);
    }
}

static void shtrih_mech_realize(SSIPeripheral *dev, Error **errp)
{
    ShtrihMechState *s = SHTRIH_MECH(dev);

    if (!s->gpio) {
        error_setg(errp, "shtrih-mech: gpio link is required");
        return;
    }
    s->cut_timer = timer_new_ms(QEMU_CLOCK_REALTIME, shtrih_mech_cut_tick, s);
    s->drv_timer = timer_new_ns(QEMU_CLOCK_REALTIME, shtrih_mech_drv_tick, s);
    s->paper_gap_timer = timer_new_ms(QEMU_CLOCK_REALTIME, shtrih_paper_gap_tick, s);
    lpc1778_gpio_add_change_handler(s->gpio, shtrih_mech_gpio_change, s);
    if (s->pwm) {
        s->pwm->match_cb = shtrih_mech_pwm_match;
        s->pwm->match_opaque = s;
    }
    memcpy(s->last_out, s->gpio->out, sizeof(s->last_out));
    memcpy(s->last_dir, s->gpio->dir, sizeof(s->last_dir));
    shtrih_mech_set_nfault(s, true);
    shtrih_mech_set_home(s, true);
    s->receipt = g_malloc0((size_t)SHTRIH_RECEIPT_MAX_ROWS * SHTRIH_RECEIPT_BYTES);
    s->receipt_n = 0;
    s->receipt_rd = 0;
    s->last_paint_row = 0;
    s->have_paint_row = false;
    s->last_ink_row = 0;
    s->have_ink_row = false;
}

static void shtrih_mech_init(Object *obj)
{
    ShtrihMechState *s = SHTRIH_MECH(obj);

    object_property_add(obj, "step-count", "uint32",
                        shtrih_mech_get_u32, NULL, NULL, &s->step_count);
    object_property_add(obj, "cut-count", "uint32",
                        shtrih_mech_get_u32, NULL, NULL, &s->cut_count);
    object_property_add(obj, "drawer-kick-count", "uint32",
                        shtrih_mech_get_u32, NULL, NULL, &s->drawer_kick_count);
    object_property_add(obj, "drawer-kick-pin", "uint32",
                        shtrih_mech_get_u32, NULL, NULL, &s->drawer_kick_pin);
    object_property_add(obj, "lines-fired", "uint32",
                        shtrih_mech_get_u32, NULL, NULL, &s->lines_fired);
    /* Head plumbing, useful when a print run produces no line at all. */
    object_property_add(obj, "head-bits", "uint32",
                        shtrih_mech_get_u32, NULL, NULL, &s->head_bits);
    object_property_add(obj, "head-words", "uint32",
                        shtrih_mech_get_u32, NULL, NULL, &s->head_words);
    object_property_add(obj, "head-latches", "uint32",
                        shtrih_mech_get_u32, NULL, NULL, &s->latch_count);
    object_property_add(obj, "head-strobes", "uint32",
                        shtrih_mech_get_u32, NULL, NULL, &s->strobe_count);
    /* Signed paper travel in 1/16-step units; negative is reverse. */
    object_property_add(obj, "paper-pos-16th", "int",
                        shtrih_mech_get_paper_pos, NULL, NULL, NULL);
    object_property_add(obj, "cutter-home", "bool",
                        shtrih_mech_get_home, NULL, NULL, NULL);
    object_property_add(obj, "stepper-enabled", "bool",
                        shtrih_mech_get_step_en, NULL, NULL, NULL);
    object_property_add(obj, "stepper-dir", "bool",
                        shtrih_mech_get_step_dir, NULL, NULL, NULL);
    object_property_add(obj, "cutter-dir", "bool",
                        shtrih_mech_get_cut_dir, NULL, NULL, NULL);
    object_property_add(obj, "last-row-hex", "string",
                        shtrih_mech_get_row, NULL, NULL, NULL);
    object_property_add(obj, "last-line-hex", "string",
                        shtrih_mech_get_line, NULL, NULL, NULL);
    object_property_add(obj, "last-line-bits", "int",
                        shtrih_mech_get_line_bits, NULL, NULL, NULL);
    object_property_add(obj, "receipt-pull", "string",
                        shtrih_mech_get_pull, NULL, NULL, NULL);
    object_property_add(obj, "gpio-rise-pull", "string",
                        shtrih_mech_get_rise, NULL, NULL, NULL);
    object_property_add(obj, "drawer-kick-trace", "string",
                        shtrih_mech_get_kick_trace, NULL, NULL, NULL);
}

static const Property shtrih_mech_properties[] = {
    DEFINE_PROP_LINK("gpio", ShtrihMechState, gpio,
                     TYPE_LPC1778_GPIO, Lpc1778GpioState *),
    DEFINE_PROP_LINK("pwm", ShtrihMechState, pwm,
                     TYPE_LPC1778_PWM, Lpc1778PwmState *),
    DEFINE_PROP_LINK("ssp", ShtrihMechState, ssp,
                     TYPE_LPC1778_SSP, Lpc1778SspState *),
};

static void shtrih_mech_finalize(Object *obj)
{
    ShtrihMechState *s = SHTRIH_MECH(obj);

    g_free(s->receipt);
    s->receipt = NULL;
}

static void shtrih_mech_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);

    k->realize = shtrih_mech_realize;
    k->transfer = shtrih_thermal_transfer;
    /* No CS on the head: the shift register follows TSCLK unconditionally. */
    k->cs_polarity = SSI_CS_NONE;
    device_class_set_legacy_reset(dc, shtrih_mech_reset);
    device_class_set_props(dc, shtrih_mech_properties);
    dc->user_creatable = false;
}

static const TypeInfo shtrih_mech_info = {
    .name = TYPE_SHTRIH_MECH,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(ShtrihMechState),
    .class_size = sizeof(SSIPeripheralClass),
    .instance_init = shtrih_mech_init,
    .instance_finalize = shtrih_mech_finalize,
    .class_init = shtrih_mech_class_init,
};

static void shtrih_mech_register_types(void)
{
    type_register_static(&shtrih_mech_info);
}

type_init(shtrih_mech_register_types)
