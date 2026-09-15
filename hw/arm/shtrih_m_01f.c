/*
 * ШТРИХ-М-01Ф (SME16031.100.00) — LPC1778FBD208
 *
 * Crystal BQ2 = 12 MHz. External SRAM R1LV0408 on EMC static CS0
 * (@0x80000000), optional DYCS SDRAM window @0xA0000000. SSP0 carries only
 * the thermal head: the W25Q80 of the schematic belongs to the WiFi board
 * SME16071 and hangs off the ESP8266, not off the LPC1778.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/boot.h"
#include "hw/arm/lpc1778_soc.h"
#include "hw/arm/shtrih_mech.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/ssi/ssi.h"
#include "qemu/error-report.h"

/* Firmware programs PLL; model CCLK as 120 MHz. Board XTAL is 12 MHz. */
#define SYSCLK_FRQ 120000000ULL

static void shtrih_m_01f_init(MachineState *machine)
{
    DeviceState *dev;
    Lpc1778SocState *soc;
    Clock *sysclk;

    sysclk = clock_new(OBJECT(machine), "SYSCLK");
    clock_set_hz(sysclk, SYSCLK_FRQ);

    dev = qdev_new(TYPE_LPC1778_SOC);
    object_property_add_child(OBJECT(machine), "soc", OBJECT(dev));
    qdev_connect_clock_in(dev, "sysclk", sysclk);
    soc = LPC1778_SOC(dev);
    if (machine->kernel_filename) {
        g_autofree char *dir = g_path_get_dirname(machine->kernel_filename);
        g_autofree char *rtc = g_build_filename(dir, "lpc1778_rtc.dat", NULL);
        g_autofree char *nvram = g_build_filename(dir, "fr_nvram.bin", NULL);
        g_autofree char *eeprom = g_build_filename(dir, "fr_nvram.bin.eeprom",
                                                   NULL);
        g_autofree char *flashp = g_build_filename(dir, "fr_nvram.bin.flash",
                                                   NULL);

        qdev_prop_set_string(dev, "nvram-path", nvram);
        qdev_prop_set_string(DEVICE(&soc->rtc), "persist-path", rtc);
        qdev_prop_set_string(DEVICE(&soc->eeprom_ctrl), "persist-path", eeprom);
        qdev_prop_set_string(DEVICE(&soc->iap), "flash-path", flashp);
    }
    {
        g_autofree char *fnlog = g_build_filename(g_get_current_dir(),
                                                  "logs", "fr_fn.log", NULL);
        g_autofree char *fnpers = NULL;

        if (machine->kernel_filename) {
            g_autofree char *dir = g_path_get_dirname(machine->kernel_filename);

            fnpers = g_build_filename(dir, "fr_fn.bin", NULL);
        } else {
            fnpers = g_build_filename(g_get_current_dir(), "fr_fn.bin", NULL);
        }
        qdev_prop_set_string(DEVICE(&soc->i2c0), "fn-log-path", fnlog);
        qdev_prop_set_string(DEVICE(&soc->i2c0), "fn-persist-path", fnpers);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    /*
     * Print mechanism: BD63510 + DRV8800 + 384-dot thermal head. The head's
     * shift register is the only thing on SSP0 (TSCLK/TSDATA = SCK0/MOSI0),
     * so the mechanism is realized on that bus. Strobes and nTLAT are GPIO.
     */
    {
        DeviceState *mech = qdev_new(TYPE_SHTRIH_MECH);
        SSIBus *ssp0 = (SSIBus *)qdev_get_child_bus(DEVICE(&soc->ssp[0]),
                                                    "ssi");

        object_property_add_child(OBJECT(machine), "mech", OBJECT(mech));
        object_property_set_link(OBJECT(mech), "gpio", OBJECT(&soc->gpio),
                                 &error_fatal);
        object_property_set_link(OBJECT(mech), "pwm", OBJECT(&soc->pwm1),
                                 &error_fatal);
        object_property_set_link(OBJECT(mech), "ssp", OBJECT(&soc->ssp[0]),
                                 &error_fatal);
        qdev_realize_and_unref(mech, BUS(ssp0), &error_fatal);
    }

    /* ISPMODE (P2.10) high = run firmware, low = DFU jumper */
    soc->gpio.in[2] |= (1u << 10);

    /*
     * The board boots from its own flash: the image (plus whatever the guest
     * programmed over IAP earlier) goes straight into the flash region, and
     * armv7m_load_kernel() is called only for the CPU reset handler.
     */
    if (machine->kernel_filename) {
        lpc1778_soc_load_flash(soc, machine->kernel_filename, &error_fatal);
    }
    armv7m_load_kernel(soc->armv7m.cpu, NULL, 0, LPC1778_FLASH_SIZE);
}

static void shtrih_m_01f_machine_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-m3"),
        NULL
    };

    mc->desc = "SHTRIH-M-01F KKT (LPC1778)";
    mc->init = shtrih_m_01f_init;
    mc->valid_cpu_types = valid_cpu_types;
}

DEFINE_MACHINE_ARM("shtrih-m-01f", shtrih_m_01f_machine_init)
