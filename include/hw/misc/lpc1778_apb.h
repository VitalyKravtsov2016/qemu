#ifndef HW_MISC_LPC1778_APB_H
#define HW_MISC_LPC1778_APB_H

#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"
#include "qom/object.h"

#define TYPE_LPC1778_CAN     "lpc1778-can"
#define TYPE_LPC1778_CANAF   "lpc1778-canaf"
#define TYPE_LPC1778_CANCR   "lpc1778-cancr"
#define TYPE_LPC1778_CANAF_RAM "lpc1778-canaf-ram"
#define TYPE_LPC1778_ENET    "lpc1778-enet"
#define TYPE_LPC1778_LCD     "lpc1778-lcd"
#define TYPE_LPC1778_I2S     "lpc1778-i2s"
#define TYPE_LPC1778_MCPWM   "lpc1778-mcpwm"
#define TYPE_LPC1778_QEI     "lpc1778-qei"
#define TYPE_LPC1778_RIT     "lpc1778-rit"

OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778CanState, LPC1778_CAN)
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778CanafState, LPC1778_CANAF)
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778CancrState, LPC1778_CANCR)
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778CanafRamState, LPC1778_CANAF_RAM)
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778EnetState, LPC1778_ENET)
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778LcdState, LPC1778_LCD)
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778I2sState, LPC1778_I2S)
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778McpwmState, LPC1778_MCPWM)
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778QeiState, LPC1778_QEI)
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778RitState, LPC1778_RIT)

#define LPC1778_APB_SIZE      0x400
#define LPC1778_ENET_SIZE     0x4000
#define LPC1778_LCD_SIZE      0x4000
#define LPC1778_CANAF_RAM_SIZE 0x800

struct Lpc1778CanState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[LPC1778_APB_SIZE / 4];
};

struct Lpc1778CanafState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[LPC1778_APB_SIZE / 4];
};

struct Lpc1778CancrState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[LPC1778_APB_SIZE / 4];
};

struct Lpc1778CanafRamState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t ram[LPC1778_CANAF_RAM_SIZE / 4];
};

struct Lpc1778EnetState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[LPC1778_ENET_SIZE / 4];
};

struct Lpc1778LcdState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[LPC1778_LCD_SIZE / 4];
};

struct Lpc1778I2sState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[LPC1778_APB_SIZE / 4];
};

struct Lpc1778McpwmState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[LPC1778_APB_SIZE / 4];
};

struct Lpc1778QeiState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[LPC1778_APB_SIZE / 4];
};

struct Lpc1778RitState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    ptimer_state *timer;
    uint32_t compval;
    uint32_t mask;
    uint32_t ctrl;
    uint32_t counter;
};

#endif
