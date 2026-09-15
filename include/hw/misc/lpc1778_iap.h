#ifndef HW_MISC_LPC1778_IAP_H
#define HW_MISC_LPC1778_IAP_H

#include "hw/core/sysbus.h"
#include "hw/misc/lpc1778_eeprom.h"
#include "qom/object.h"

#define TYPE_LPC1778_IAP "lpc1778-iap"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778IapState, LPC1778_IAP)

struct Lpc1778IapState {
    SysBusDevice parent_obj;
    MemoryRegion bootrom;
    MemoryRegion doorbell;
    MemoryRegion *flash;
    Lpc1778EepromState *eeprom;
    char *flash_path;
};

void lpc1778_iap_connect(Lpc1778IapState *s, MemoryRegion *flash,
                         Lpc1778EepromState *eeprom);
void lpc1778_iap_load_persist(Lpc1778IapState *s);
void lpc1778_iap_save_flash(Lpc1778IapState *s);

#endif
