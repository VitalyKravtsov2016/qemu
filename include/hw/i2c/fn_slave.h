#ifndef HW_I2C_FN_SLAVE_H
#define HW_I2C_FN_SLAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define FN_RX_MAX 1100
#define FN_TLV_MAX 1024
#define FN_ARCHIVE_MAX 64

typedef struct FnArchivedDoc {
    uint8_t type;
    uint8_t ofd_ack;
    uint32_t fd_no;
    uint32_t fp;
    uint16_t shift_no;
    uint8_t op_type;
    uint32_t amount_kop;
} FnArchivedDoc;

typedef struct FnSlave {
    char sn[17];
    char inn[13];
    char rnm[21];
    char ofd_inn[13];
    uint8_t tax_system;
    uint8_t work_mode;
    uint8_t ext_flags;
    uint8_t ffd_version;
    uint32_t reason_code;
    uint8_t last_dt[5];
    bool fiscal_open;
    bool shift_open;
    uint8_t life_phase;
    uint8_t cur_doc;
    uint8_t doc_data;
    uint8_t ofd_transport;
    uint8_t ofd_reading;
    uint32_t last_fd_no;
    uint32_t shift_number;
    uint32_t receipt_in_shift;
    uint32_t last_fp;
    uint8_t doc_tlv[FN_TLV_MAX];
    unsigned doc_tlv_len;
    uint8_t reg_tlv[FN_TLV_MAX];
    unsigned reg_tlv_len;

    FnArchivedDoc archive[FN_ARCHIVE_MAX];
    unsigned archive_n;
    uint32_t ofd_queue[FN_ARCHIVE_MAX];
    unsigned ofd_qn;
    unsigned ofd_read_off;
    uint8_t ofd_msg[256];
    unsigned ofd_msg_len;

    bool marking;
    uint8_t km_phase; /* 1 none, 2 after B1, 3 after B5, 4 after B6 */
    uint8_t km_saved;
    uint8_t notif_xfer; /* 0 idle, 1 reading, 2 wait ticket */
    uint8_t tlv_read_kind; /* 0 off, 1 cmd 46, 2 cmd 47 */
    unsigned tlv_read_off;

    uint8_t rx[FN_RX_MAX];
    unsigned rx_len;
    unsigned rx_pos;
    int busy_nack_left;

    uint8_t last_cmd;
    char *log_path;
    FILE *log;
    bool log_open_failed;
    char *persist_path;
} FnSlave;

void fn_slave_cold_init(FnSlave *fn);
void fn_slave_reset(FnSlave *fn);
void fn_slave_set_log_path(FnSlave *fn, const char *path);
void fn_slave_set_persist_path(FnSlave *fn, const char *path);
void fn_slave_flush(FnSlave *fn);
void fn_slave_close_log(FnSlave *fn);
void fn_slave_on_master_write(FnSlave *fn, const uint8_t *data, size_t len);
bool fn_slave_pop_rx(FnSlave *fn, uint8_t *out);
bool fn_slave_busy(const FnSlave *fn);
void fn_slave_note_address_probe(FnSlave *fn, bool is_read);

#endif
