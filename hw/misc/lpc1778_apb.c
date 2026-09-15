/*
 * Remaining LPC1778 APB/AHB blocks (UM10470) so later firmware can probe
 * the full map without hanging on unimp reads of 0.
 *
 * Idle / reset-value models: CAN, CAN AF, Ethernet MAC, LCD, I2S, MCPWM,
 * QEI, RIT. IRQs stay deasserted unless the block is actually running.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/lpc1778_apb.h"
#include "hw/core/irq.h"
#include "hw/core/ptimer.h"
#include "qapi/error.h"
#include "qemu/module.h"

/* ---- helpers ---------------------------------------------------------- */

static uint32_t *reg32(uint32_t *regs, hwaddr addr)
{
    return &regs[addr / 4];
}

/* ---- CAN controller --------------------------------------------------- */

#define CAN_MOD  0x00
#define CAN_CMR  0x04
#define CAN_GSR  0x08
#define CAN_ICR  0x0C
#define CAN_IER  0x10
#define CAN_SR   0x1C

#define CAN_MOD_RM   (1u << 0)
#define CAN_CMR_TR   (1u << 0)
#define CAN_CMR_AT   (1u << 1)
#define CAN_CMR_RRB  (1u << 2)
#define CAN_CMR_CDO  (1u << 3)
#define CAN_GSR_RBS  (1u << 0)
#define CAN_GSR_DOS  (1u << 1)
#define CAN_GSR_TBS  (1u << 2)
#define CAN_GSR_TCS  (1u << 3)
#define CAN_ICR_TI1  (1u << 1)

static void lpc1778_can_update_irq(Lpc1778CanState *s)
{
    uint32_t icr = *reg32(s->regs, CAN_ICR);
    uint32_t ier = *reg32(s->regs, CAN_IER);

    qemu_set_irq(s->irq, (icr & ier) != 0);
}

static uint64_t lpc1778_can_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778CanState *s = opaque;
    uint32_t v;

    if (addr >= LPC1778_APB_SIZE) {
        return 0;
    }
    v = *reg32(s->regs, addr);
    if (addr == CAN_ICR) {
        /* ICR is read-clear. */
        *reg32(s->regs, CAN_ICR) = 0;
        lpc1778_can_update_irq(s);
    }
    return v;
}

static void lpc1778_can_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778CanState *s = opaque;
    uint32_t v = (uint32_t)value;

    if (addr >= LPC1778_APB_SIZE) {
        return;
    }
    switch (addr) {
    case CAN_CMR:
        if (v & CAN_CMR_TR) {
            /* Instant TX complete, all three buffers free. */
            *reg32(s->regs, CAN_GSR) |= CAN_GSR_TBS | CAN_GSR_TCS;
            *reg32(s->regs, CAN_SR) |= 0x00000CCC;
            *reg32(s->regs, CAN_ICR) |= CAN_ICR_TI1;
            lpc1778_can_update_irq(s);
        }
        if (v & CAN_CMR_RRB) {
            *reg32(s->regs, CAN_GSR) &= ~CAN_GSR_RBS;
        }
        if (v & CAN_CMR_CDO) {
            *reg32(s->regs, CAN_GSR) &= ~CAN_GSR_DOS;
        }
        break;
    case CAN_GSR:
    case CAN_ICR:
    case CAN_SR:
        /* Read-only (ICR is read-clear). */
        break;
    default:
        *reg32(s->regs, addr) = v;
        if (addr == CAN_IER) {
            lpc1778_can_update_irq(s);
        }
        break;
    }
}

static const MemoryRegionOps lpc1778_can_ops = {
    .read = lpc1778_can_read,
    .write = lpc1778_can_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_can_reset(DeviceState *dev)
{
    Lpc1778CanState *s = LPC1778_CAN(dev);

    memset(s->regs, 0, sizeof(s->regs));
    *reg32(s->regs, CAN_MOD) = CAN_MOD_RM;
    *reg32(s->regs, CAN_GSR) = 0x3C; /* TBS, TCS, RS, TS */
    *reg32(s->regs, CAN_SR) = 0x00000CCC;
    lpc1778_can_update_irq(s);
}

static void lpc1778_can_init(Object *obj)
{
    Lpc1778CanState *s = LPC1778_CAN(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_can_ops, s,
                          TYPE_LPC1778_CAN, LPC1778_APB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_can_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), lpc1778_can_reset);
}

/* ---- generic register file ------------------------------------------- */

#define REGS_DEVICE(prefix, TypeName, CAST, typestr, size, reset_fn)           \
static uint64_t prefix##_read(void *opaque, hwaddr addr, unsigned sz)          \
{                                                                              \
    TypeName *s = opaque;                                                      \
    (void)sz;                                                                  \
    if (addr >= (size)) {                                                      \
        return 0;                                                              \
    }                                                                          \
    return s->regs[addr / 4];                                                  \
}                                                                              \
static void prefix##_write(void *opaque, hwaddr addr,                          \
                           uint64_t value, unsigned sz)                        \
{                                                                              \
    TypeName *s = opaque;                                                      \
    (void)sz;                                                                  \
    if (addr >= (size)) {                                                      \
        return;                                                                \
    }                                                                          \
    s->regs[addr / 4] = (uint32_t)value;                                       \
}                                                                              \
static const MemoryRegionOps prefix##_ops = {                                  \
    .read = prefix##_read,                                                     \
    .write = prefix##_write,                                                   \
    .endianness = DEVICE_LITTLE_ENDIAN,                                        \
};                                                                             \
static void prefix##_init(Object *obj)                                         \
{                                                                              \
    TypeName *s = CAST(obj);                                                   \
    memory_region_init_io(&s->iomem, obj, &prefix##_ops, s,                    \
                          typestr, (size));                                    \
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);                          \
}                                                                              \
static void prefix##_class_init(ObjectClass *klass, const void *data)          \
{                                                                              \
    device_class_set_legacy_reset(DEVICE_CLASS(klass), reset_fn);              \
}

static void lpc1778_canaf_reset(DeviceState *dev)
{
    Lpc1778CanafState *s = LPC1778_CANAF(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0] = 1; /* AFMR AccOff */
}

static void lpc1778_cancr_reset(DeviceState *dev)
{
    memset(LPC1778_CANCR(dev)->regs, 0, sizeof(LPC1778_CANCR(dev)->regs));
}

static void lpc1778_canaf_ram_reset(DeviceState *dev)
{
    memset(LPC1778_CANAF_RAM(dev)->ram, 0, sizeof(LPC1778_CANAF_RAM(dev)->ram));
}

static uint64_t lpc1778_canaf_ram_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778CanafRamState *s = opaque;

    (void)size;
    if (addr >= LPC1778_CANAF_RAM_SIZE) {
        return 0;
    }
    return s->ram[addr / 4];
}

static void lpc1778_canaf_ram_write(void *opaque, hwaddr addr,
                                    uint64_t value, unsigned size)
{
    Lpc1778CanafRamState *s = opaque;

    (void)size;
    if (addr >= LPC1778_CANAF_RAM_SIZE) {
        return;
    }
    s->ram[addr / 4] = (uint32_t)value;
}

static const MemoryRegionOps lpc1778_canaf_ram_ops = {
    .read = lpc1778_canaf_ram_read,
    .write = lpc1778_canaf_ram_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_canaf_ram_init(Object *obj)
{
    Lpc1778CanafRamState *s = LPC1778_CANAF_RAM(obj);

    memory_region_init_io(&s->iomem, obj, &lpc1778_canaf_ram_ops, s,
                          TYPE_LPC1778_CANAF_RAM, LPC1778_CANAF_RAM_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_canaf_ram_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), lpc1778_canaf_ram_reset);
}

REGS_DEVICE(lpc1778_canaf, Lpc1778CanafState, LPC1778_CANAF,
            TYPE_LPC1778_CANAF, LPC1778_APB_SIZE, lpc1778_canaf_reset)
REGS_DEVICE(lpc1778_cancr, Lpc1778CancrState, LPC1778_CANCR,
            TYPE_LPC1778_CANCR, LPC1778_APB_SIZE, lpc1778_cancr_reset)

/* ---- Ethernet MAC ----------------------------------------------------- */

#define ENET_MAC1  0x000
#define ENET_MCMD  0x024
#define ENET_MADR  0x028
#define ENET_MWTD  0x02C
#define ENET_MRDD  0x030
#define ENET_MIND  0x034
#define ENET_CMD   0x100
#define ENET_STAT  0x104
#define ENET_RXPROD 0x114
#define ENET_RXCONS 0x118
#define ENET_TXPROD 0x128
#define ENET_TXCONS 0x12C

#define MAC1_RESETS 0xCF00
#define CMD_RESETS  0x38 /* TxReset | RxReset | RegReset */

static uint16_t enet_mdio_read(uint32_t adreg)
{
    uint32_t reg = adreg & 0x1F;

    switch (reg) {
    case 0:
        return 0x3100; /* BMCR */
    case 1:
        return 0x782D; /* BMSR: link up */
    case 2:
        return 0x0022; /* PHYIDR1 */
    case 3:
        return 0x1619; /* PHYIDR2 */
    default:
        return 0xFFFF;
    }
}

static uint64_t lpc1778_enet_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778EnetState *s = opaque;

    (void)size;
    if (addr >= LPC1778_ENET_SIZE) {
        return 0;
    }
    if (addr == ENET_MIND) {
        return 0; /* MDIO not busy */
    }
    return s->regs[addr / 4];
}

static void lpc1778_enet_write(void *opaque, hwaddr addr,
                               uint64_t value, unsigned size)
{
    Lpc1778EnetState *s = opaque;
    uint32_t v = (uint32_t)value;

    (void)size;
    if (addr >= LPC1778_ENET_SIZE) {
        return;
    }
    switch (addr) {
    case ENET_MAC1:
        s->regs[addr / 4] = v & ~MAC1_RESETS;
        break;
    case ENET_CMD:
        s->regs[addr / 4] = v & ~CMD_RESETS;
        break;
    case ENET_MCMD:
        s->regs[addr / 4] = 0;
        if (v & 1) {
            s->regs[ENET_MRDD / 4] = enet_mdio_read(s->regs[ENET_MADR / 4]);
        }
        break;
    case ENET_TXPROD:
        s->regs[addr / 4] = v;
        /* Consume immediately: TX descriptors are "done". */
        s->regs[ENET_TXCONS / 4] = v;
        break;
    case ENET_MIND:
    case ENET_MRDD:
    case ENET_STAT:
        break;
    default:
        s->regs[addr / 4] = v;
        if (addr == ENET_RXCONS) {
            s->regs[ENET_RXPROD / 4] = v;
        }
        break;
    }
}

static const MemoryRegionOps lpc1778_enet_ops = {
    .read = lpc1778_enet_read,
    .write = lpc1778_enet_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_enet_reset(DeviceState *dev)
{
    Lpc1778EnetState *s = LPC1778_ENET(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void lpc1778_enet_init(Object *obj)
{
    Lpc1778EnetState *s = LPC1778_ENET(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_enet_ops, s,
                          TYPE_LPC1778_ENET, LPC1778_ENET_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_enet_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), lpc1778_enet_reset);
}

/* ---- LCD -------------------------------------------------------------- */

#define LCD_UPBASE 0x010
#define LCD_UPCURR 0x02C

static uint64_t lpc1778_lcd_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778LcdState *s = opaque;

    (void)size;
    if (addr >= LPC1778_LCD_SIZE) {
        return 0;
    }
    if (addr == LCD_UPCURR) {
        return s->regs[LCD_UPBASE / 4];
    }
    return s->regs[addr / 4];
}

static void lpc1778_lcd_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778LcdState *s = opaque;

    (void)size;
    if (addr >= LPC1778_LCD_SIZE) {
        return;
    }
    s->regs[addr / 4] = (uint32_t)value;
}

static const MemoryRegionOps lpc1778_lcd_ops = {
    .read = lpc1778_lcd_read,
    .write = lpc1778_lcd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_lcd_reset(DeviceState *dev)
{
    Lpc1778LcdState *s = LPC1778_LCD(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void lpc1778_lcd_init(Object *obj)
{
    Lpc1778LcdState *s = LPC1778_LCD(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_lcd_ops, s,
                          TYPE_LPC1778_LCD, LPC1778_LCD_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_lcd_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), lpc1778_lcd_reset);
}

/* ---- I2S -------------------------------------------------------------- */

#define I2S_DAO   0x00
#define I2S_DAI   0x04
#define I2S_STATE 0x10

static uint64_t lpc1778_i2s_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778I2sState *s = opaque;

    (void)size;
    if (addr >= LPC1778_APB_SIZE) {
        return 0;
    }
    return s->regs[addr / 4];
}

static void lpc1778_i2s_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778I2sState *s = opaque;

    (void)size;
    if (addr >= LPC1778_APB_SIZE) {
        return;
    }
    s->regs[addr / 4] = (uint32_t)value;
}

static const MemoryRegionOps lpc1778_i2s_ops = {
    .read = lpc1778_i2s_read,
    .write = lpc1778_i2s_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_i2s_reset(DeviceState *dev)
{
    Lpc1778I2sState *s = LPC1778_I2S(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[I2S_DAO / 4] = 0x87E1;
    s->regs[I2S_DAI / 4] = 0x87E1;
    qemu_set_irq(s->irq, 0);
}

static void lpc1778_i2s_init(Object *obj)
{
    Lpc1778I2sState *s = LPC1778_I2S(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_i2s_ops, s,
                          TYPE_LPC1778_I2S, LPC1778_APB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_i2s_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), lpc1778_i2s_reset);
}

/* ---- MCPWM (CON_SET / CON_CLR) ---------------------------------------- */

static uint64_t lpc1778_mcpwm_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778McpwmState *s = opaque;

    (void)size;
    if (addr >= LPC1778_APB_SIZE) {
        return 0;
    }
    return s->regs[addr / 4];
}

static void lpc1778_mcpwm_write(void *opaque, hwaddr addr,
                                uint64_t value, unsigned size)
{
    Lpc1778McpwmState *s = opaque;
    uint32_t v = (uint32_t)value;
    uint32_t base;

    (void)size;
    if (addr >= LPC1778_APB_SIZE) {
        return;
    }
    /* SET at +4, CLR at +8 relative to CON/CAPCON/INTEN/CNTCON/INTF. */
    if ((addr & 0xC) == 0x4 && addr < 0x40) {
        base = addr & ~0xCu;
        s->regs[base / 4] |= v;
        return;
    }
    if ((addr & 0xC) == 0x8 && addr < 0x40) {
        base = addr & ~0xCu;
        s->regs[base / 4] &= ~v;
        return;
    }
    s->regs[addr / 4] = v;
}

static const MemoryRegionOps lpc1778_mcpwm_ops = {
    .read = lpc1778_mcpwm_read,
    .write = lpc1778_mcpwm_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_mcpwm_reset(DeviceState *dev)
{
    Lpc1778McpwmState *s = LPC1778_MCPWM(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void lpc1778_mcpwm_init(Object *obj)
{
    Lpc1778McpwmState *s = LPC1778_MCPWM(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_mcpwm_ops, s,
                          TYPE_LPC1778_MCPWM, LPC1778_APB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_mcpwm_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), lpc1778_mcpwm_reset);
}

/* ---- QEI -------------------------------------------------------------- */

#define QEI_CON  0x00
#define QEI_STAT 0x04
#define QEI_POS  0x0C

static uint64_t lpc1778_qei_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778QeiState *s = opaque;

    (void)size;
    if (addr >= LPC1778_APB_SIZE) {
        return 0;
    }
    return s->regs[addr / 4];
}

static void lpc1778_qei_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778QeiState *s = opaque;
    uint32_t v = (uint32_t)value;

    (void)size;
    if (addr >= LPC1778_APB_SIZE) {
        return;
    }
    if (addr == QEI_CON) {
        if (v & 1) {
            s->regs[QEI_POS / 4] = 0;
        }
        return;
    }
    if (addr == QEI_STAT) {
        s->regs[addr / 4] &= ~v;
        return;
    }
    s->regs[addr / 4] = v;
}

static const MemoryRegionOps lpc1778_qei_ops = {
    .read = lpc1778_qei_read,
    .write = lpc1778_qei_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_qei_reset(DeviceState *dev)
{
    Lpc1778QeiState *s = LPC1778_QEI(dev);

    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void lpc1778_qei_init(Object *obj)
{
    Lpc1778QeiState *s = LPC1778_QEI(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_qei_ops, s,
                          TYPE_LPC1778_QEI, LPC1778_APB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_qei_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), lpc1778_qei_reset);
}

/* ---- RIT -------------------------------------------------------------- */

#define RIT_CTRL_INT  (1u << 0)
#define RIT_CTRL_ENCLR (1u << 1)
#define RIT_CTRL_EN   (1u << 3)

static void lpc1778_rit_update_irq(Lpc1778RitState *s)
{
    qemu_set_irq(s->irq, (s->ctrl & RIT_CTRL_INT) != 0);
}

static void lpc1778_rit_rearm(Lpc1778RitState *s)
{
    if (!s->timer) {
        return;
    }
    ptimer_transaction_begin(s->timer);
    if (s->ctrl & RIT_CTRL_EN) {
        ptimer_set_limit(s->timer, 1, 1);
        ptimer_run(s->timer, 0);
    } else {
        ptimer_stop(s->timer);
    }
    ptimer_transaction_commit(s->timer);
}

static void lpc1778_rit_tick(void *opaque)
{
    Lpc1778RitState *s = opaque;
    uint32_t masked;

    if (!(s->ctrl & RIT_CTRL_EN)) {
        return;
    }
    s->counter++;
    masked = ~s->mask;
    if ((s->counter & masked) == (s->compval & masked)) {
        s->ctrl |= RIT_CTRL_INT;
        if (s->ctrl & RIT_CTRL_ENCLR) {
            s->counter = 0;
        }
        lpc1778_rit_update_irq(s);
    }
}

static uint64_t lpc1778_rit_read(void *opaque, hwaddr addr, unsigned size)
{
    Lpc1778RitState *s = opaque;

    (void)size;
    switch (addr) {
    case 0x00:
        return s->compval;
    case 0x04:
        return s->mask;
    case 0x08:
        return s->ctrl;
    case 0x0C:
        return s->counter;
    default:
        return 0;
    }
}

static void lpc1778_rit_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778RitState *s = opaque;
    uint32_t v = (uint32_t)value;

    (void)size;
    switch (addr) {
    case 0x00:
        s->compval = v;
        break;
    case 0x04:
        s->mask = v;
        break;
    case 0x08:
        if (v & RIT_CTRL_INT) {
            s->ctrl &= ~RIT_CTRL_INT;
        }
        s->ctrl = (s->ctrl & RIT_CTRL_INT) | (v & (RIT_CTRL_ENCLR | RIT_CTRL_EN | (1u << 2)));
        lpc1778_rit_update_irq(s);
        lpc1778_rit_rearm(s);
        break;
    case 0x0C:
        s->counter = v;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps lpc1778_rit_ops = {
    .read = lpc1778_rit_read,
    .write = lpc1778_rit_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void lpc1778_rit_reset(DeviceState *dev)
{
    Lpc1778RitState *s = LPC1778_RIT(dev);

    s->compval = 0xFFFFFFFF;
    s->mask = 0;
    s->ctrl = 0;
    s->counter = 0;
    lpc1778_rit_update_irq(s);
    lpc1778_rit_rearm(s);
}

static void lpc1778_rit_realize(DeviceState *dev, Error **errp)
{
    Lpc1778RitState *s = LPC1778_RIT(dev);

    (void)errp;
    s->timer = ptimer_init(lpc1778_rit_tick, s, PTIMER_POLICY_LEGACY);
    ptimer_transaction_begin(s->timer);
    ptimer_set_freq(s->timer, 1000000);
    ptimer_transaction_commit(s->timer);
}

static void lpc1778_rit_init(Object *obj)
{
    Lpc1778RitState *s = LPC1778_RIT(obj);

    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &lpc1778_rit_ops, s,
                          TYPE_LPC1778_RIT, LPC1778_APB_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static void lpc1778_rit_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, lpc1778_rit_reset);
    dc->realize = lpc1778_rit_realize;
}

/* ---- type registration ------------------------------------------------ */

static const TypeInfo lpc1778_apb_types[] = {
    {
        .name = TYPE_LPC1778_CAN,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Lpc1778CanState),
        .instance_init = lpc1778_can_init,
        .class_init = lpc1778_can_class_init,
    },
    {
        .name = TYPE_LPC1778_CANAF,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Lpc1778CanafState),
        .instance_init = lpc1778_canaf_init,
        .class_init = lpc1778_canaf_class_init,
    },
    {
        .name = TYPE_LPC1778_CANCR,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Lpc1778CancrState),
        .instance_init = lpc1778_cancr_init,
        .class_init = lpc1778_cancr_class_init,
    },
    {
        .name = TYPE_LPC1778_CANAF_RAM,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Lpc1778CanafRamState),
        .instance_init = lpc1778_canaf_ram_init,
        .class_init = lpc1778_canaf_ram_class_init,
    },
    {
        .name = TYPE_LPC1778_ENET,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Lpc1778EnetState),
        .instance_init = lpc1778_enet_init,
        .class_init = lpc1778_enet_class_init,
    },
    {
        .name = TYPE_LPC1778_LCD,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Lpc1778LcdState),
        .instance_init = lpc1778_lcd_init,
        .class_init = lpc1778_lcd_class_init,
    },
    {
        .name = TYPE_LPC1778_I2S,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Lpc1778I2sState),
        .instance_init = lpc1778_i2s_init,
        .class_init = lpc1778_i2s_class_init,
    },
    {
        .name = TYPE_LPC1778_MCPWM,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Lpc1778McpwmState),
        .instance_init = lpc1778_mcpwm_init,
        .class_init = lpc1778_mcpwm_class_init,
    },
    {
        .name = TYPE_LPC1778_QEI,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Lpc1778QeiState),
        .instance_init = lpc1778_qei_init,
        .class_init = lpc1778_qei_class_init,
    },
    {
        .name = TYPE_LPC1778_RIT,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(Lpc1778RitState),
        .instance_init = lpc1778_rit_init,
        .class_init = lpc1778_rit_class_init,
    },
};

DEFINE_TYPES(lpc1778_apb_types)
