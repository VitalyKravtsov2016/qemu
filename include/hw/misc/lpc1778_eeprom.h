#ifndef HW_MISC_LPC1778_EEPROM_H
#define HW_MISC_LPC1778_EEPROM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "qemu/notify.h"

#define TYPE_LPC1778_EEPROM "lpc1778-eeprom"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778EepromState, LPC1778_EEPROM)

#define LPC1778_EEPROM_MEM_SIZE 4096

struct Lpc1778EepromState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    char *persist_path;
    Notifier exit;
    uint8_t mem[LPC1778_EEPROM_MEM_SIZE];
    uint32_t cmd;
    uint32_t addr;
    uint32_t rdata;
    uint32_t wstate;
    uint32_t clkdiv;
    uint32_t pwrdwn;
    uint32_t inten;
    uint32_t intstat;
};

void lpc1778_eeprom_persist(Lpc1778EepromState *s);

#endif
