#ifndef HW_MISC_LPC1778_USB_H
#define HW_MISC_LPC1778_USB_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_LPC1778_USB "lpc1778-usb"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778UsbState, LPC1778_USB)

#define LPC1778_USB_SIZE 0x4000

struct Lpc1778UsbState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[LPC1778_USB_SIZE / 4];
};

#endif
