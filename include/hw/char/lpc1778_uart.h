#ifndef HW_CHAR_LPC1778_UART_H
#define HW_CHAR_LPC1778_UART_H

#include "hw/core/sysbus.h"
#include "chardev/char-fe.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_LPC1778_UART "lpc1778-uart"
OBJECT_DECLARE_SIMPLE_TYPE(Lpc1778UartState, LPC1778_UART)

#define LPC1778_UART_FIFO 16

struct Lpc1778UartState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq irq;
    uint8_t rbr;
    uint8_t ier;
    uint8_t iir;
    uint8_t lcr;
    uint8_t mcr;
    uint8_t lsr;
    uint8_t msr;
    uint8_t scr;
    uint8_t dll;
    uint8_t dlm;
    uint8_t fcr;
    uint32_t fdr;
    uint8_t ter;
    uint8_t rx_fifo[LPC1778_UART_FIFO];
    unsigned rx_len;
    unsigned rx_r;
    bool thr_ipending;
    uint8_t tx_shift;
    bool tx_shift_busy;
    QEMUBH *irq_bh;
    QEMUTimer *thre_timer;
    QEMUTimer *rx_timer;
    bool diag;
};

#endif
