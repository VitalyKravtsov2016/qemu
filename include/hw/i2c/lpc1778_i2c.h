#ifndef HW_I2C_LPC1778_I2C_H
#define HW_I2C_LPC1778_I2C_H

#include "hw/core/sysbus.h"
#include "hw/i2c/fn_slave.h"
#include "qom/object.h"

#define TYPE_LPC1778_I2C "lpc1778-i2c"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778I2cState, LPC1778_I2C)

#define LPC1778_I2C_TX_MAX FN_RX_MAX

struct Lpc1778I2cState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;

    uint32_t con;
    uint32_t stat;
    uint32_t dat;
    uint32_t adr0;
    uint32_t sclh;
    uint32_t scll;

    enum {
        I2C_IDLE = 0,
        I2C_START,
        I2C_ADDR_W,
        I2C_ADDR_R,
        I2C_TX,
        I2C_RX,
    } phase;
    bool aa;
    bool pending_dat;
    uint8_t pending_byte;
    uint8_t tx[LPC1778_I2C_TX_MAX];
    unsigned tx_len;

    FnSlave fn;
    bool fn_slave;
    char *fn_log_path;
    char *fn_persist_path;
};

#endif
