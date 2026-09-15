/*
 * NXP LPC1778 IAP (UM10470) — real Copy/Erase/EEPROM, not a success stub.
 *
 * Firmware trampoline @0x19218 does bx 0x1FFF1FF1. Boot ROM code there
 * rings a doorbell MMIO; the C handler programs on-chip flash RAM.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/misc/lpc1778_iap.h"
#include "hw/core/cpu.h"
#include "hw/core/qdev-properties.h"
#include "exec/translation-block.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "system/dma.h"
#include "target/arm/cpu.h"

#define IAP_FLASH_SIZE (512 * 1024)

#define IAP_CMD_SUCCESS 0
#define IAP_INVALID_SECTOR 7
#define IAP_SECTOR_NOT_BLANK 8
#define IAP_COUNT_ERROR 6
#define IAP_DST_ADDR_ERROR 3
#define IAP_SRC_ADDR_ERROR 2
#define IAP_COMPARE_ERROR 10

#define IAP_PREPARE      50
#define IAP_COPY         51
#define IAP_ERASE        52
#define IAP_BLANK        53
#define IAP_PARTID       54
#define IAP_BOOTVER      55
#define IAP_COMPARE      56
#define IAP_READ_UID     58
#define IAP_ERASE_PAGE   59
#define IAP_EEPROM_WRITE 61
#define IAP_EEPROM_READ  62

#define LPC1778_PART_ID  0x281D3F47u
#define LPC1778_BOOT_VER 0x00010101u

static uint32_t lpc1778_sector_base(uint32_t sector)
{
    if (sector < 16u) {
        return sector * 0x1000u;
    }
    return 0x10000u + (sector - 16u) * 0x8000u;
}

static uint32_t lpc1778_sector_size(uint32_t sector)
{
    return sector < 16u ? 0x1000u : 0x8000u;
}

static uint8_t *iap_flash_ptr(Lpc1778IapState *s)
{
    if (!s->flash) {
        return NULL;
    }
    return memory_region_get_ram_ptr(s->flash);
}

static void iap_invalidate(hwaddr start, hwaddr len)
{
    if (current_cpu && len) {
        tb_invalidate_phys_range(current_cpu, start, start + len - 1);
    }
}

void lpc1778_iap_save_flash(Lpc1778IapState *s)
{
    uint8_t *ptr;
    g_autofree char *tmp = NULL;
    FILE *f;

    if (!s->flash_path || !s->flash_path[0]) {
        return;
    }
    ptr = iap_flash_ptr(s);
    if (!ptr) {
        return;
    }
    tmp = g_strdup_printf("%s.tmp", s->flash_path);
    f = fopen(tmp, "wb");
    if (!f) {
        error_report("on-chip flash: cannot write %s", tmp);
        return;
    }
    if (fwrite(ptr, 1, IAP_FLASH_SIZE, f) != IAP_FLASH_SIZE) {
        error_report("on-chip flash: short write to %s", tmp);
        fclose(f);
        unlink(tmp);
        return;
    }
    fclose(f);
    unlink(s->flash_path);
    if (rename(tmp, s->flash_path) != 0) {
        error_report("on-chip flash: rename %s -> %s failed", tmp, s->flash_path);
        unlink(tmp);
        return;
    }
}

void lpc1778_iap_load_persist(Lpc1778IapState *s)
{
    uint8_t *ptr;
    g_autofree uint8_t *saved = NULL;
    FILE *f;
    size_t n;

    if (!s->flash_path || !s->flash_path[0]) {
        return;
    }
    ptr = iap_flash_ptr(s);
    if (!ptr) {
        return;
    }
    f = fopen(s->flash_path, "rb");
    if (!f) {
        info_report("on-chip flash: no persist file, using firmware image");
        return;
    }
    saved = g_malloc(IAP_FLASH_SIZE);
    n = fread(saved, 1, IAP_FLASH_SIZE, f);
    fclose(f);
    if (n < 256) {
        error_report("on-chip flash: persist %s too small (%zu)", s->flash_path, n);
        return;
    }
    /* Same firmware image → keep IAP-programmed sectors. Otherwise drop. */
    if (memcmp(saved, ptr, 256) != 0) {
        info_report("on-chip flash: firmware image changed, ignoring %s",
                    s->flash_path);
        return;
    }
    memcpy(ptr, saved, n);
    iap_invalidate(0, n);
    info_report("on-chip flash: loaded %zu bytes from %s", n, s->flash_path);
}

static void iap_run(Lpc1778IapState *s, uint32_t cmd_ptr, uint32_t status_ptr)
{
    uint32_t cmd[5] = {0};
    uint32_t status[4] = {0};
    uint8_t *flash = iap_flash_ptr(s);
    uint32_t op, s0, s1, dst, src, nbytes, i;
    bool dirty = false;

    if (cmd_ptr) {
        address_space_read(&address_space_memory, cmd_ptr,
                           MEMTXATTRS_UNSPECIFIED, cmd, sizeof(cmd));
    }
    op = cmd[0];
    status[0] = IAP_CMD_SUCCESS;
    info_report("IAP cmd=%u p1=%08x p2=%08x p3=%08x",
                op, cmd[1], cmd[2], cmd[3]);

    switch (op) {
    case IAP_PREPARE:
    case IAP_ERASE_PAGE:
        break;

    case IAP_COPY:
        dst = cmd[1];
        src = cmd[2];
        nbytes = cmd[3];
        if (!flash || nbytes == 0 || dst >= IAP_FLASH_SIZE ||
            nbytes > IAP_FLASH_SIZE || dst + nbytes > IAP_FLASH_SIZE) {
            status[0] = IAP_DST_ADDR_ERROR;
            break;
        }
        {
            g_autofree uint8_t *buf = g_malloc(nbytes);

            address_space_read(&address_space_memory, src,
                               MEMTXATTRS_UNSPECIFIED, buf, nbytes);
            memcpy(flash + dst, buf, nbytes);
            iap_invalidate(dst, nbytes);
            dirty = true;
            info_report("IAP copy RAM 0x%08x -> flash 0x%08x n=%u",
                        src, dst, nbytes);
        }
        break;

    case IAP_ERASE:
        s0 = cmd[1];
        s1 = cmd[2];
        if (!flash || s1 < s0 || s1 > 29u) {
            status[0] = IAP_INVALID_SECTOR;
            break;
        }
        for (i = s0; i <= s1; i++) {
            uint32_t base = lpc1778_sector_base(i);
            uint32_t len = lpc1778_sector_size(i);

            memset(flash + base, 0xFF, len);
            iap_invalidate(base, len);
        }
        dirty = true;
        info_report("IAP erase sectors %u..%u", s0, s1);
        break;

    case IAP_BLANK:
        s0 = cmd[1];
        s1 = cmd[2];
        if (!flash || s1 < s0 || s1 > 29u) {
            status[0] = IAP_INVALID_SECTOR;
            break;
        }
        for (i = s0; i <= s1; i++) {
            uint32_t base = lpc1778_sector_base(i);
            uint32_t len = lpc1778_sector_size(i);
            uint32_t off;

            for (off = 0; off < len; off++) {
                if (flash[base + off] != 0xFF) {
                    status[0] = IAP_SECTOR_NOT_BLANK;
                    status[1] = base + off;
                    status[2] = flash[base + off];
                    goto done;
                }
            }
        }
        break;

    case IAP_PARTID:
        status[1] = LPC1778_PART_ID;
        break;

    case IAP_BOOTVER:
        status[1] = LPC1778_BOOT_VER;
        break;

    case IAP_COMPARE:
        dst = cmd[1];
        src = cmd[2];
        nbytes = cmd[3];
        if (!nbytes) {
            status[0] = IAP_COUNT_ERROR;
            break;
        }
        {
            g_autofree uint8_t *a = g_malloc(nbytes);
            g_autofree uint8_t *b = g_malloc(nbytes);
            uint32_t off;

            address_space_read(&address_space_memory, dst,
                               MEMTXATTRS_UNSPECIFIED, a, nbytes);
            address_space_read(&address_space_memory, src,
                               MEMTXATTRS_UNSPECIFIED, b, nbytes);
            for (off = 0; off < nbytes; off++) {
                if (a[off] != b[off]) {
                    status[0] = IAP_COMPARE_ERROR;
                    status[1] = off;
                    goto done;
                }
            }
        }
        break;

    case IAP_READ_UID:
        status[1] = 0x53485452u; /* 'SHTR' */
        status[2] = 0x49482D4Du;
        status[3] = 0x30314600u;
        break;

    case IAP_EEPROM_WRITE:
        if (!s->eeprom) {
            status[0] = IAP_DST_ADDR_ERROR;
            break;
        }
        dst = cmd[1];
        src = cmd[2];
        nbytes = cmd[3];
        if (nbytes == 0 || dst >= LPC1778_EEPROM_MEM_SIZE ||
            nbytes > LPC1778_EEPROM_MEM_SIZE ||
            dst + nbytes > LPC1778_EEPROM_MEM_SIZE) {
            status[0] = IAP_DST_ADDR_ERROR;
            break;
        }
        address_space_read(&address_space_memory, src,
                           MEMTXATTRS_UNSPECIFIED,
                           s->eeprom->mem + dst, nbytes);
        lpc1778_eeprom_persist(s->eeprom);
        break;

    case IAP_EEPROM_READ:
        if (!s->eeprom) {
            status[0] = IAP_SRC_ADDR_ERROR;
            break;
        }
        src = cmd[1];
        dst = cmd[2];
        nbytes = cmd[3];
        if (nbytes == 0 || src >= LPC1778_EEPROM_MEM_SIZE ||
            nbytes > LPC1778_EEPROM_MEM_SIZE ||
            src + nbytes > LPC1778_EEPROM_MEM_SIZE) {
            status[0] = IAP_SRC_ADDR_ERROR;
            break;
        }
        address_space_write(&address_space_memory, dst,
                            MEMTXATTRS_UNSPECIFIED,
                            s->eeprom->mem + src, nbytes);
        break;

    default:
        qemu_log_mask(LOG_UNIMP, "lpc1778 IAP cmd %u not implemented\n", op);
        break;
    }

done:
    if (status_ptr) {
        address_space_write(&address_space_memory, status_ptr,
                            MEMTXATTRS_UNSPECIFIED, status, sizeof(status));
    }
    if (dirty) {
        lpc1778_iap_save_flash(s);
    }
}

static uint64_t lpc1778_iap_read(void *opaque, hwaddr addr, unsigned size)
{
    (void)opaque;
    (void)addr;
    (void)size;
    return 0;
}

static void lpc1778_iap_write(void *opaque, hwaddr addr,
                              uint64_t value, unsigned size)
{
    Lpc1778IapState *s = opaque;
    ARMCPU *cpu;
    uint32_t cmd_ptr, status_ptr;

    (void)addr;
    (void)value;
    (void)size;
    if (!current_cpu) {
        return;
    }
    cpu = ARM_CPU(current_cpu);
    cmd_ptr = cpu->env.regs[0];
    status_ptr = cpu->env.regs[1];
    iap_run(s, cmd_ptr, status_ptr);
}

static const MemoryRegionOps lpc1778_iap_ops = {
    .read = lpc1778_iap_read,
    .write = lpc1778_iap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
};

void lpc1778_iap_connect(Lpc1778IapState *s, MemoryRegion *flash,
                         Lpc1778EepromState *eeprom)
{
    s->flash = flash;
    s->eeprom = eeprom;
}

static void lpc1778_iap_init(Object *obj)
{
    Lpc1778IapState *s = LPC1778_IAP(obj);

    /* 16 KiB Boot ROM window @0x1FFF0000 (page-aligned). Entry 0x1FFF1FF1. */
    memory_region_init_ram(&s->bootrom, obj, "lpc1778.bootrom", 0x4000,
                           &error_fatal);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->bootrom);

    memory_region_init_io(&s->doorbell, obj, &lpc1778_iap_ops, s,
                          TYPE_LPC1778_IAP, 0x10);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->doorbell);
}

static void lpc1778_iap_realize(DeviceState *dev, Error **errp)
{
    Lpc1778IapState *s = LPC1778_IAP(dev);
    uint8_t *rom = memory_region_get_ram_ptr(&s->bootrom);
    /*
     * Thumb @0x1FFF1FF1:
     *   ldr r2, [pc, #4]   ; 0x1FFF0000 doorbell
     *   str r0, [r2]
     *   bx lr
     */
    static const uint8_t boot[] = {
        0x01, 0x4A,
        0x10, 0x60,
        0x70, 0x47,
        0x00, 0xBF,
        0x00, 0x00, 0xFF, 0x1F,
    };

    (void)errp;
    memset(rom, 0xFF, 0x4000);
    memcpy(rom + 0x1FF0, boot, sizeof(boot));
}

static const Property lpc1778_iap_properties[] = {
    DEFINE_PROP_STRING("flash-path", Lpc1778IapState, flash_path),
};

static void lpc1778_iap_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = lpc1778_iap_realize;
    device_class_set_props(dc, lpc1778_iap_properties);
}

static const TypeInfo lpc1778_iap_info = {
    .name = TYPE_LPC1778_IAP,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Lpc1778IapState),
    .instance_init = lpc1778_iap_init,
    .class_init = lpc1778_iap_class_init,
};

static void lpc1778_iap_register_types(void)
{
    type_register_static(&lpc1778_iap_info);
}

type_init(lpc1778_iap_register_types)
