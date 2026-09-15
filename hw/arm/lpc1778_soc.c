/*
 * NXP LPC1778 SoC
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "system/address-spaces.h"
#include "system/system.h"
#include "hw/arm/lpc1778_soc.h"
#include "hw/core/cpu.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/loader.h"
#include "hw/misc/unimp.h"
#include "qemu/error-report.h"
#include "qemu/timer.h"
#include "system/dma.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "cpu.h"

#define FLASH_BASE   0x00000000
#define EEPROM_BASE  0x00200000
#define SRAM_BASE    0x10000000
#define AHB_BASE     0x20000000
#define CRC_BASE     0x20090000
#define GPIO_BASE    0x20098000
#define EMC_BASE     0x2009C000
#define SYSCON_BASE  0x400FC000
#define IOCON_BASE   0x4002C000
#define SDRAM_BASE   0xA0000000
#define SDRAM_SIZE   (16 * 1024 * 1024)
#define EXT_SRAM_BASE 0x80000000ull

static const hwaddr uart_addr[LPC1778_NUM_UARTS] = {
    0x4000C000, 0x40010000, 0x40098000, 0x4009C000, 0x400A4000,
};
static const int uart_irq[LPC1778_NUM_UARTS] = { 5, 6, 7, 8, 35 };

static const hwaddr ssp_addr[3] = { 0x40088000, 0x40030000, 0x400AC000 };
static const int ssp_irq[3] = { 14, 15, 36 };

static const hwaddr timer_addr[4] = {
    0x40004000, 0x40008000, 0x40090000, 0x40094000,
};
/* LPC1778: TIMER0..3 → IRQ 1..4 */
static const int timer_irq[4] = { 1, 2, 3, 4 };

static void lpc1778_soc_save_nvram(Lpc1778SocState *s)
{
    void *ptr;
    g_autofree char *tmp = NULL;
    FILE *f;

    if (!s->nvram_path || !s->nvram_path[0]) {
        return;
    }
    ptr = memory_region_get_ram_ptr(&s->ext_sram);
    if (!ptr) {
        return;
    }
    tmp = g_strdup_printf("%s.tmp", s->nvram_path);
    f = fopen(tmp, "wb");
    if (!f) {
        error_report("battery SRAM CS0: cannot write %s", tmp);
        return;
    }
    if (fwrite(ptr, 1, LPC1778_EXT_SRAM_SIZE, f) != LPC1778_EXT_SRAM_SIZE) {
        error_report("battery SRAM CS0: short write to %s", tmp);
        fclose(f);
        unlink(tmp);
        return;
    }
    fclose(f);
    unlink(s->nvram_path);
    if (rename(tmp, s->nvram_path) != 0) {
        error_report("battery SRAM CS0: rename %s -> %s failed", tmp,
                     s->nvram_path);
        unlink(tmp);
        return;
    }
}

static void lpc1778_soc_load_nvram(Lpc1778SocState *s)
{
    void *ptr;
    FILE *f;
    size_t n;

    if (!s->nvram_path || !s->nvram_path[0]) {
        return;
    }
    ptr = memory_region_get_ram_ptr(&s->ext_sram);
    if (!ptr) {
        return;
    }
    f = fopen(s->nvram_path, "rb");
    if (!f) {
        info_report("battery SRAM CS0: empty (no %s)", s->nvram_path);
        return;
    }
    n = fread(ptr, 1, LPC1778_EXT_SRAM_SIZE, f);
    fclose(f);
    info_report("battery SRAM CS0: loaded %zu bytes from %s", n, s->nvram_path);
}

static void lpc1778_soc_nvram_exit(Notifier *n, void *opaque)
{
    Lpc1778SocState *s = container_of(n, Lpc1778SocState, nvram_exit);

    (void)opaque;
    lpc1778_soc_save_nvram(s);
    lpc1778_iap_save_flash(&s->iap);
}

/*
 * On-chip flash is written into the RAM-backed region directly instead of
 * through the ROM loader: rom_reset() re-applies ROM blobs on every system
 * reset, which would erase the sectors the guest programmed over IAP. Real
 * flash keeps its contents across a core reset.
 */
void lpc1778_soc_load_flash(Lpc1778SocState *s, const char *filename,
                            Error **errp)
{
    uint8_t *ptr = memory_region_get_ram_ptr(&s->flash);
    ssize_t n;

    if (!ptr) {
        error_setg(errp, "lpc1778: no flash memory");
        return;
    }
    memset(ptr, 0xFF, LPC1778_FLASH_SIZE);
    n = load_image_size(filename, ptr, LPC1778_FLASH_SIZE);
    if (n < 0) {
        error_setg(errp, "lpc1778: cannot load flash image %s", filename);
        return;
    }
    lpc1778_iap_load_persist(&s->iap);
}

static void lpc1778_soc_nvram_tick(void *opaque)
{
    Lpc1778SocState *s = opaque;

    /*
     * REALTIME, not VIRTUAL: with -icount the guest WFI warps virtual time
     * to the next timer, so a VIRTUAL persist tick becomes a 1 MiB fwrite
     * storm and UART/ENQ starve.
     */
    lpc1778_soc_save_nvram(s);
    if (s->nvram_timer) {
        timer_mod(s->nvram_timer,
                  qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 8000);
    }
}

static void lpc1778_soc_log_reset(void *opaque)
{
    (void)opaque;
    error_report("lpc1778: system_reset");
}

/*
 * AIRCR.SYSRESETREQ is a CPU pin. Log architectural NVIC/SCB state only —
 * not guest SRAM or firmware objects.
 */
static void lpc1778_soc_sysresetreq(void *opaque, int n, int level)
{
    Lpc1778SocState *s = opaque;
    ARMCPU *cpu;
    CPUARMState *env;

    (void)n;
    if (!level) {
        return;
    }
    cpu = s->armv7m.cpu;
    env = &cpu->env;
    error_report("lpc1778: SYSRESETREQ pc=%08x lr=%08x sp=%08x "
                 "r0=%08x r1=%08x r2=%08x r3=%08x "
                 "cfsr=%08x hfsr=%08x bfar=%08x mmfar=%08x "
                 "basepri=%02x exc=%u control=%02x",
                 env->regs[15], env->regs[14], env->regs[13],
                 env->regs[0], env->regs[1], env->regs[2], env->regs[3],
                 env->v7m.cfsr[M_REG_NS], env->v7m.hfsr, env->v7m.bfar,
                 env->v7m.mmfar[M_REG_NS],
                 env->v7m.basepri[M_REG_NS], env->v7m.exception,
                 env->v7m.control[M_REG_NS]);
    /* Debug: stacked frame / caller of NVIC_SystemReset (Thumb LR). */
    {
        uint32_t sp = env->regs[13];
        uint32_t w[8];
        int i;
        MemTxResult res;

        for (i = 0; i < 8; i++) {
            res = address_space_read(&address_space_memory, sp + i * 4,
                                     MEMTXATTRS_UNSPECIFIED, &w[i], 4);
            if (res != MEMTX_OK) {
                w[i] = 0xFFFFFFFF;
            }
        }
        error_report("lpc1778: SYSRESETREQ stack %08x: "
                     "%08x %08x %08x %08x %08x %08x %08x %08x",
                     sp, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
    lpc1778_syscon_note_reset(&s->syscon, LPC1778_RSID_SYSRESET);
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}

static void lpc1778_soc_initfn(Object *obj)
{
    Lpc1778SocState *s = LPC1778_SOC(obj);
    int i;

    object_initialize_child(obj, "armv7m", &s->armv7m, TYPE_ARMV7M);
    object_initialize_child(obj, "syscon", &s->syscon, TYPE_LPC1778_SYSCON);
    object_initialize_child(obj, "gpio", &s->gpio, TYPE_LPC1778_GPIO);
    object_initialize_child(obj, "iocon", &s->iocon, TYPE_LPC1778_IOCON);
    object_initialize_child(obj, "crc", &s->crc, TYPE_LPC1778_CRC);
    object_initialize_child(obj, "emc", &s->emc, TYPE_LPC1778_EMC);
    object_initialize_child(obj, "gpdma", &s->gpdma, TYPE_LPC1778_GPDMA);
    object_initialize_child(obj, "i2c0", &s->i2c0, TYPE_LPC1778_I2C);
    object_initialize_child(obj, "i2c1", &s->i2c1, TYPE_LPC1778_I2C);
    object_initialize_child(obj, "i2c2", &s->i2c2, TYPE_LPC1778_I2C);
    object_initialize_child(obj, "rtc", &s->rtc, TYPE_LPC1778_RTC);
    object_initialize_child(obj, "usb", &s->usb, TYPE_LPC1778_USB);
    object_initialize_child(obj, "adc", &s->adc, TYPE_LPC1778_ADC);
    object_initialize_child(obj, "dac", &s->dac, TYPE_LPC1778_DAC);
    object_initialize_child(obj, "eeprom", &s->eeprom_ctrl, TYPE_LPC1778_EEPROM);
    object_initialize_child(obj, "iap", &s->iap, TYPE_LPC1778_IAP);
    object_initialize_child(obj, "wdt", &s->wdt, TYPE_LPC1778_WDT);
    object_initialize_child(obj, "gpioint", &s->gpioint, TYPE_LPC1778_GPIOINT);
    object_initialize_child(obj, "can1", &s->can[0], TYPE_LPC1778_CAN);
    object_initialize_child(obj, "can2", &s->can[1], TYPE_LPC1778_CAN);
    object_initialize_child(obj, "canaf", &s->canaf, TYPE_LPC1778_CANAF);
    object_initialize_child(obj, "cancr", &s->cancr, TYPE_LPC1778_CANCR);
    object_initialize_child(obj, "canaf-ram", &s->canaf_ram, TYPE_LPC1778_CANAF_RAM);
    object_initialize_child(obj, "enet", &s->enet, TYPE_LPC1778_ENET);
    object_initialize_child(obj, "lcd", &s->lcd, TYPE_LPC1778_LCD);
    object_initialize_child(obj, "i2s", &s->i2s, TYPE_LPC1778_I2S);
    object_initialize_child(obj, "mcpwm", &s->mcpwm, TYPE_LPC1778_MCPWM);
    object_initialize_child(obj, "qei", &s->qei, TYPE_LPC1778_QEI);
    object_initialize_child(obj, "rit", &s->rit, TYPE_LPC1778_RIT);

    for (i = 0; i < LPC1778_NUM_UARTS; i++) {
        object_initialize_child(obj, "uart[*]", &s->uart[i], TYPE_LPC1778_UART);
    }
    for (i = 0; i < 3; i++) {
        object_initialize_child(obj, "ssp[*]", &s->ssp[i], TYPE_LPC1778_SSP);
    }
    for (i = 0; i < 4; i++) {
        object_initialize_child(obj, "timer[*]", &s->timer[i], TYPE_LPC1778_TIMER);
    }
    object_initialize_child(obj, "pwm0", &s->pwm0, TYPE_LPC1778_PWM);
    object_initialize_child(obj, "pwm1", &s->pwm1, TYPE_LPC1778_PWM);

    s->sysclk = qdev_init_clock_in(DEVICE(s), "sysclk", NULL, NULL, 0);
    s->refclk = qdev_init_clock_in(DEVICE(s), "refclk", NULL, NULL, 0);
}

static void lpc1778_map_ext_sram_cs0(Lpc1778SocState *s, Error **errp)
{
    MemoryRegion *system_memory = get_system_memory();
    Error *err = NULL;
    int i;

    memory_region_init_ram(&s->ext_sram, NULL, "lpc1778.ext-sram-cs0",
                           LPC1778_EXT_SRAM_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(system_memory, EXT_SRAM_BASE, &s->ext_sram);

    /* A[18:0] only → mirror physical chip across the 64 MB CS0 window. */
    s->ext_sram_alias = g_new0(MemoryRegion, LPC1778_EXT_SRAM_ALIASES - 1);
    for (i = 1; i < LPC1778_EXT_SRAM_ALIASES; i++) {
        g_autofree char *name = g_strdup_printf("lpc1778.ext-sram-cs0.alias[%d]", i);
        memory_region_init_alias(&s->ext_sram_alias[i - 1], NULL, name,
                                 &s->ext_sram, 0, LPC1778_EXT_SRAM_SIZE);
        memory_region_add_subregion(system_memory,
                                    EXT_SRAM_BASE +
                                        (hwaddr)i * LPC1778_EXT_SRAM_SIZE,
                                    &s->ext_sram_alias[i - 1]);
    }

    /* R1LV0408 is battery-backed: keep contents across QEMU process restarts. */
    lpc1778_soc_load_nvram(s);
    s->nvram_exit.notify = lpc1778_soc_nvram_exit;
    qemu_add_exit_notifier(&s->nvram_exit);
    s->nvram_timer = timer_new_ms(QEMU_CLOCK_REALTIME, lpc1778_soc_nvram_tick, s);
    timer_mod(s->nvram_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + 8000);
}

static bool lpc1778_map(SysBusDevice *sbd, hwaddr base, Error **errp)
{
    if (!sysbus_realize(sbd, errp)) {
        return false;
    }
    sysbus_mmio_map(sbd, 0, base);
    return true;
}

static bool lpc1778_map_irq(SysBusDevice *sbd, hwaddr base,
                            DeviceState *armv7m, int irq, Error **errp)
{
    if (!lpc1778_map(sbd, base, errp)) {
        return false;
    }
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, irq));
    return true;
}

static void lpc1778_soc_realize(DeviceState *dev_soc, Error **errp)
{
    Lpc1778SocState *s = LPC1778_SOC(dev_soc);
    MemoryRegion *system_memory = get_system_memory();
    DeviceState *armv7m, *dev;
    SysBusDevice *busdev;
    Error *err = NULL;
    int i;

    if (clock_has_source(s->refclk)) {
        error_setg(errp, "refclk clock must not be wired up by the board code");
        return;
    }
    if (!clock_has_source(s->sysclk)) {
        error_setg(errp, "sysclk clock must be wired up by the board code");
        return;
    }

    clock_set_mul_div(s->refclk, 8, 1);
    clock_set_source(s->refclk, s->sysclk);

    memory_region_init_ram(&s->flash, OBJECT(dev_soc), "lpc1778.flash",
                           LPC1778_FLASH_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(system_memory, FLASH_BASE, &s->flash);

    memory_region_init_ram(&s->sram, NULL, "lpc1778.sram",
                           LPC1778_SRAM_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(system_memory, SRAM_BASE, &s->sram);

    memory_region_init_ram(&s->ahb_sram, NULL, "lpc1778.ahb-sram",
                           LPC1778_AHB_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(system_memory, AHB_BASE, &s->ahb_sram);

    /*
     * UM10470: 32 KiB AHB SRAM at 0x20000000. The next 32 KiB is reserved.
     * Guest bignum (license ECDSA) addresses the last limb as
     * mpi + 0x1000001C, which for a local-SRAM mpi is 0x2000xxxx. Mapping
     * that window onto the upper half of local SRAM makes the cell match.
     */
    memory_region_init_alias(&s->ahb_sram_hi, NULL, "lpc1778.ahb-sram-hi",
                             &s->sram, LPC1778_AHB_SIZE, LPC1778_AHB_SIZE);
    memory_region_add_subregion(system_memory, AHB_BASE + LPC1778_AHB_SIZE,
                                &s->ahb_sram_hi);

    memory_region_init_ram(&s->sdram, NULL, "lpc1778.sdram",
                           SDRAM_SIZE, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }
    memory_region_add_subregion(system_memory, SDRAM_BASE, &s->sdram);

    lpc1778_map_ext_sram_cs0(s, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    armv7m = DEVICE(&s->armv7m);
    qdev_prop_set_uint32(armv7m, "num-irq", LPC1778_NUM_IRQ);
    /* LPC1778 CMSIS: __NVIC_PRIO_BITS = 5. */
    qdev_prop_set_uint8(armv7m, "num-prio-bits", 5);
    qdev_prop_set_string(armv7m, "cpu-type", ARM_CPU_TYPE_NAME("cortex-m3"));
    qdev_prop_set_bit(armv7m, "enable-bitband", true);
    qdev_connect_clock_in(armv7m, "cpuclk", s->sysclk);
    qdev_connect_clock_in(armv7m, "refclk", s->refclk);
    object_property_set_link(OBJECT(&s->armv7m), "memory",
                             OBJECT(system_memory), &error_abort);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->armv7m), errp)) {
        return;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->syscon), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->syscon), 0, SYSCON_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->iocon), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->iocon), 0, IOCON_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpio), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio), 0, GPIO_BASE);

    s->gpioint.gpio = &s->gpio;
    if (!lpc1778_map_irq(SYS_BUS_DEVICE(&s->gpioint), 0x40028000, armv7m, 38,
                         errp)) {
        return;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->crc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->crc), 0, CRC_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->emc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->emc), 0, EMC_BASE);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gpdma), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpdma), 0, 0x20080000);
    /* LPC1778 DMA IRQ = 26 */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpdma), 0,
                       qdev_get_gpio_in(armv7m, 26));

    qdev_prop_set_bit(DEVICE(&s->i2c0), "fn-slave", true);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->i2c0), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2c0), 0, 0x4001C000);
    /* LPC1778 I2C0 IRQ = 10 */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->i2c0), 0,
                       qdev_get_gpio_in(armv7m, 10));

    /*
     * I2C1 @ 0x4005C000. App IRQ 11 (@0x4FEB0) is a real driver: STAT 0/0xF8
     * are filtered, anything else is dispatched to 0x19190 with r0=1.
     * Unimplemented STAT=0 never reached that path. Do not leave this as unimp.
     */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->i2c1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2c1), 0, 0x4005C000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->i2c1), 0,
                       qdev_get_gpio_in(armv7m, 11));

    if (!lpc1778_map_irq(SYS_BUS_DEVICE(&s->i2c2), 0x400A0000, armv7m, 12,
                         errp)) {
        return;
    }

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->rtc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->rtc), 0, 0x40024000);
    /* LPC1778 RTC IRQ = 17 */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->rtc), 0, qdev_get_gpio_in(armv7m, 17));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->usb), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->usb), 0, 0x2008C000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->usb), 0, qdev_get_gpio_in(armv7m, 24));

    s->adc.gpio = &s->gpio;
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->adc), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->adc), 0, 0x40034000);
    /* LPC1778 ADC IRQ = 22 */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->adc), 0,
                       qdev_get_gpio_in(armv7m, 22));

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->dac), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dac), 0, 0x4008C000);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->eeprom_ctrl), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->eeprom_ctrl), 0, EEPROM_BASE);

    lpc1778_iap_connect(&s->iap, &s->flash, &s->eeprom_ctrl);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->iap), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->iap), 0, 0x1FFF0000);
    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&s->iap), 1, 0x1FFF0000, 1);

    for (i = 0; i < 4; i++) {
        s->timer[i].syscon = &s->syscon;
        s->timer[i].cclk_hz = clock_get_hz(s->sysclk);
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->timer[i]), errp)) {
            return;
        }
        busdev = SYS_BUS_DEVICE(&s->timer[i]);
        sysbus_mmio_map(busdev, 0, timer_addr[i]);
        /* TIMER0..3 → IRQ 1..4. TIMER3 (IRQ 4) is the mech/paper-feed tick. */
        sysbus_connect_irq(busdev, 0, qdev_get_gpio_in(armv7m, timer_irq[i]));
    }

    /*
     * PWM1 @ 0x40018000. Do not connect IRQ 9: the app vector is the
     * default handler (infinite loop). Firmware polls PWM IR instead.
     * PWM0 IRQ 39 is wired; the line stays low unless PWM0 is enabled.
     */
    s->pwm0.syscon = &s->syscon;
    s->pwm1.syscon = &s->syscon;
    s->pwm0.cclk_hz = clock_get_hz(s->sysclk);
    s->pwm1.cclk_hz = clock_get_hz(s->sysclk);
    if (!lpc1778_map_irq(SYS_BUS_DEVICE(&s->pwm0), 0x40014000, armv7m, 39,
                         errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->pwm1), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pwm1), 0, 0x40018000);

    for (i = 0; i < LPC1778_NUM_UARTS; i++) {
        dev = DEVICE(&s->uart[i]);
        qdev_prop_set_chr(dev, "chardev", serial_hd(i));
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->uart[i]), errp)) {
            return;
        }
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_mmio_map(busdev, 0, uart_addr[i]);
        sysbus_connect_irq(busdev, 0, qdev_get_gpio_in(armv7m, uart_irq[i]));
    }

    for (i = 0; i < 3; i++) {
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->ssp[i]), errp)) {
            return;
        }
        busdev = SYS_BUS_DEVICE(&s->ssp[i]);
        sysbus_mmio_map(busdev, 0, ssp_addr[i]);
        sysbus_connect_irq(busdev, 0, qdev_get_gpio_in(armv7m, ssp_irq[i]));
    }

    s->wdt.syscon = &s->syscon;
    if (!lpc1778_map_irq(SYS_BUS_DEVICE(&s->wdt), 0x40000000, armv7m, 0, errp)) {
        return;
    }
    if (!lpc1778_map(SYS_BUS_DEVICE(&s->canaf_ram), 0x40038000, errp) ||
        !lpc1778_map(SYS_BUS_DEVICE(&s->canaf), 0x4003C000, errp) ||
        !lpc1778_map(SYS_BUS_DEVICE(&s->cancr), 0x40040000, errp) ||
        !lpc1778_map_irq(SYS_BUS_DEVICE(&s->can[0]), 0x40044000, armv7m, 25,
                         errp) ||
        !lpc1778_map_irq(SYS_BUS_DEVICE(&s->can[1]), 0x40048000, armv7m, 25,
                         errp) ||
        !lpc1778_map_irq(SYS_BUS_DEVICE(&s->enet), 0x20084000, armv7m, 28,
                         errp) ||
        !lpc1778_map_irq(SYS_BUS_DEVICE(&s->lcd), 0x20088000, armv7m, 37,
                         errp) ||
        !lpc1778_map_irq(SYS_BUS_DEVICE(&s->i2s), 0x400A8000, armv7m, 27,
                         errp) ||
        !lpc1778_map(SYS_BUS_DEVICE(&s->rit), 0x400B0000, errp) ||
        !lpc1778_map_irq(SYS_BUS_DEVICE(&s->mcpwm), 0x400B8000, armv7m, 30,
                         errp) ||
        !lpc1778_map_irq(SYS_BUS_DEVICE(&s->qei), 0x400BC000, armv7m, 31,
                         errp)) {
        return;
    }

    /* MCI is ARM PL181; without a card, commands complete with CMDTIMEOUT. */
    {
        DeviceState *mci = qdev_new("pl181");
        SysBusDevice *sbd = SYS_BUS_DEVICE(mci);

        if (!sysbus_realize(sbd, errp)) {
            return;
        }
        sysbus_mmio_map(sbd, 0, 0x400C0000);
        /* LPC1778 single MCI IRQ = 29; both PL181 lines share it. */
        sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(armv7m, 29));
        sysbus_connect_irq(sbd, 1, qdev_get_gpio_in(armv7m, 29));
    }

    /*
     * Unicorn maps whole APB/AHB/PPB as RAM so unmapped probes return 0.
     * QEMU raises BusFault on holes; MemManage/BusFault/UsageFault all call
     * NVIC_SystemReset (0x31f58). Fill gaps under existing devices (prio -1000).
     */
    create_unimplemented_device("lpc1778.apb-holes", 0x40000000, 0x100000);
    create_unimplemented_device("lpc1778.ahb-periph-holes", 0x20080000, 0x80000);
    create_unimplemented_device("lpc1778.bootrom-holes", 0x1FFF0000, 0x10000);
    create_unimplemented_device("lpc1778.ppb-holes", 0xE0000000, 0x100000);
    create_unimplemented_device("lpc1778.emc-cs1-holes", 0x90000000, 0x10000000);

    qdev_connect_gpio_out_named(armv7m, "SYSRESETREQ", 0,
                                qemu_allocate_irq(lpc1778_soc_sysresetreq, s, 0));

    qemu_register_reset(lpc1778_soc_log_reset, s);
}

static const Property lpc1778_soc_properties[] = {
    DEFINE_PROP_STRING("nvram-path", Lpc1778SocState, nvram_path),
};

static void lpc1778_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = lpc1778_soc_realize;
    device_class_set_props(dc, lpc1778_soc_properties);
}

static const TypeInfo lpc1778_soc_info = {
    .name = TYPE_LPC1778_SOC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778SocState),
    .instance_init = lpc1778_soc_initfn,
    .class_init = lpc1778_soc_class_init,
};

static void lpc1778_soc_register_types(void)
{
    type_register_static(&lpc1778_soc_info);
}

type_init(lpc1778_soc_register_types)
