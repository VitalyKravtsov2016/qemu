#ifndef HW_ARM_LPC1778_SOC_H
#define HW_ARM_LPC1778_SOC_H

#include "hw/arm/armv7m.h"
#include "hw/adc/lpc1778_adc.h"
#include "hw/char/lpc1778_uart.h"
#include "hw/gpio/lpc1778_gpio.h"
#include "hw/i2c/lpc1778_i2c.h"
#include "hw/misc/lpc1778_crc.h"
#include "hw/misc/lpc1778_dac.h"
#include "hw/misc/lpc1778_eeprom.h"
#include "hw/misc/lpc1778_emc.h"
#include "hw/misc/lpc1778_gpdma.h"
#include "hw/misc/lpc1778_iocon.h"
#include "hw/misc/lpc1778_rtc.h"
#include "hw/misc/lpc1778_syscon.h"
#include "hw/misc/lpc1778_usb.h"
#include "hw/misc/lpc1778_iap.h"
#include "hw/misc/lpc1778_wdt.h"
#include "hw/misc/lpc1778_apb.h"
#include "hw/gpio/lpc1778_gpioint.h"
#include "hw/ssi/lpc1778_ssp.h"
#include "hw/timer/lpc1778_pwm.h"
#include "hw/timer/lpc1778_timer.h"
#include "qom/object.h"
#include "qemu/notify.h"
#include "qemu/timer.h"

#define TYPE_LPC1778_SOC "lpc1778-soc"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778SocState, LPC1778_SOC)

#define LPC1778_FLASH_SIZE  (512 * 1024)
#define LPC1778_SRAM_SIZE   (64 * 1024)
#define LPC1778_AHB_SIZE    (32 * 1024)
#define LPC1778_EEPROM_SIZE (4 * 1024)
#define LPC1778_NUM_UARTS   5
#define LPC1778_NUM_IRQ     41
#define LPC1778_EXT_SRAM_SIZE   (512 * 1024)
#define LPC1778_EXT_SRAM_WINDOW (64 * 1024 * 1024)
#define LPC1778_EXT_SRAM_ALIASES \
    (LPC1778_EXT_SRAM_WINDOW / LPC1778_EXT_SRAM_SIZE)

struct Lpc1778SocState {
    SysBusDevice parent_obj;

    ARMv7MState armv7m;
    Lpc1778SysconState syscon;
    Lpc1778UartState uart[LPC1778_NUM_UARTS];
    Lpc1778GpioState gpio;
    Lpc1778IoconState iocon;
    Lpc1778CrcState crc;
    Lpc1778EmcState emc;
    Lpc1778GpdmaState gpdma;
    Lpc1778I2cState i2c0;
    Lpc1778I2cState i2c1;
    Lpc1778I2cState i2c2;
    Lpc1778TimerState timer[4];
    Lpc1778PwmState pwm0;
    Lpc1778PwmState pwm1;
    Lpc1778RtcState rtc;
    Lpc1778UsbState usb;
    Lpc1778AdcState adc;
    Lpc1778DacState dac;
    Lpc1778EepromState eeprom_ctrl;
    Lpc1778IapState iap;
    Lpc1778WdtState wdt;
    Lpc1778GpioIntState gpioint;
    Lpc1778CanState can[2];
    Lpc1778CanafState canaf;
    Lpc1778CancrState cancr;
    Lpc1778CanafRamState canaf_ram;
    Lpc1778EnetState enet;
    Lpc1778LcdState lcd;
    Lpc1778I2sState i2s;
    Lpc1778McpwmState mcpwm;
    Lpc1778QeiState qei;
    Lpc1778RitState rit;
    Lpc1778SspState ssp[3];

    MemoryRegion flash;
    MemoryRegion sram;
    MemoryRegion ahb_sram;
    /* 0x20008000..0x2000FFFF → local SRAM 0x10008000.. (see lpc1778_soc.c). */
    MemoryRegion ahb_sram_hi;
    MemoryRegion sdram;
    /* R1LV0408 on EMC static CS0 @ 0x80000000, A[18:0] mirrored.
     * Battery-backed: persist to nvram-path across QEMU launches. */
    MemoryRegion ext_sram;
    MemoryRegion *ext_sram_alias;
    char *nvram_path;
    Notifier nvram_exit;
    QEMUTimer *nvram_timer;

    Clock *sysclk;
    Clock *refclk;
};

void lpc1778_soc_load_flash(Lpc1778SocState *s, const char *filename,
                            Error **errp);

#endif
