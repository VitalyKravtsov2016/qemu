#ifndef HW_MISC_LPC1778_SYSCON_H
#define HW_MISC_LPC1778_SYSCON_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_LPC1778_SYSCON "lpc1778-syscon"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778SysconState, LPC1778_SYSCON)

#define LPC1778_SYSCON_SIZE 0x200

struct Lpc1778SysconState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[LPC1778_SYSCON_SIZE / 4];
    uint8_t pll0_feed;
    uint8_t pll1_feed;
    uint32_t rsid;
    uint32_t pending_rsid;
};

#define LPC1778_RSID_POR      (1u << 0)
#define LPC1778_RSID_EXTR     (1u << 1)
#define LPC1778_RSID_WDTR     (1u << 2)
#define LPC1778_RSID_BODR     (1u << 3)
#define LPC1778_RSID_SYSRESET (1u << 4)
#define LPC1778_RSID_LOCKUP   (1u << 5)

void lpc1778_syscon_note_reset(Lpc1778SysconState *s, uint32_t rsid_bits);
uint32_t lpc1778_syscon_pclk_hz(Lpc1778SysconState *s, uint32_t cclk_hz);

#endif
