#ifndef HW_ARM_SHTRIH_MECH_H
#define HW_ARM_SHTRIH_MECH_H

#include "hw/core/sysbus.h"
#include "hw/gpio/lpc1778_gpio.h"
#include "hw/ssi/lpc1778_ssp.h"
#include "hw/ssi/ssi.h"
#include "hw/timer/lpc1778_pwm.h"
#include "qom/object.h"
#include "qemu/timer.h"

#define TYPE_SHTRIH_MECH "shtrih-mech"
OBJECT_DECLARE_SIMPLE_TYPE(ShtrihMechState, SHTRIH_MECH)

#define SHTRIH_THERMAL_DOTS  384
#define SHTRIH_THERMAL_BYTES ((SHTRIH_THERMAL_DOTS + 7) / 8)
/*
 * Firmware clocks 656 bits/line. The physical head is 384 heaters, but the
 * receipt view stores the full shift so double-width text is not cropped.
 */
#define SHTRIH_RECEIPT_DOTS  656
#define SHTRIH_RECEIPT_BYTES ((SHTRIH_RECEIPT_DOTS + 7) / 8)
#define SHTRIH_RECEIPT_MAX_ROWS 8192
/* A line may clock in more than the dot count; keep it all to measure. */
#define SHTRIH_HEAD_MAX_BITS 1024
#define SHTRIH_HEAD_MAX_BYTES (SHTRIH_HEAD_MAX_BITS / 8)

struct ShtrihMechState {
    SSIPeripheral parent_obj;

    Lpc1778GpioState *gpio;
    Lpc1778PwmState *pwm;
    /* SSP0 drives TSCLK/TSDATA; its word length decides how many dots shift. */
    Lpc1778SspState *ssp;
    QEMUTimer *cut_timer;
    QEMUTimer *drv_timer;
    QEMUTimer *paper_gap_timer;
    bool paper_was;
    bool paper_gap_on;
    bool paper_gap_armed;
    int64_t paper_gap_deadline_ms;

    uint32_t last_out[LPC1778_GPIO_PORTS];
    uint32_t last_dir[LPC1778_GPIO_PORTS];

    uint32_t step_count;
    /* Paper position in 1/16-step units so every BD63510 mode is exact. */
    int32_t paper_pos;
    /* DRV8800 charge pump is not ready until this deadline (nSLEEP L→H). */
    int64_t drv_wake_ns;
    uint32_t cut_count;
    uint32_t drawer_kick_count;
    uint32_t drawer_kick_pin;
    int64_t drawer_edge_ns;
    bool drawer_pin_high;
    uint32_t drawer_w_ms[8];
    uint32_t drawer_g_ms[8];
    uint32_t drawer_wn;
    uint32_t drawer_gn;
    uint32_t gpio_rise[LPC1778_GPIO_PORTS];
    uint32_t lines_fired;
    bool cutter_away;
    uint8_t cut_phase;

    uint8_t shift[SHTRIH_HEAD_MAX_BYTES];
    uint8_t latched[SHTRIH_HEAD_MAX_BYTES];
    int bit_pos;
    int line_bits;
    bool latched_valid;
    bool line_pending;
    uint32_t head_bits;
    uint32_t head_words;
    uint32_t latch_count;
    uint32_t strobe_count;

    uint8_t *receipt;
    uint32_t receipt_n;
    uint32_t receipt_rd;
    /* Paper row of the last receipt line, in full-step units. */
    int32_t last_paint_row;
    bool have_paint_row;
    /* Last row that actually burned dots; blank feed past this is capped. */
    int32_t last_ink_row;
    bool have_ink_row;
};

#endif
