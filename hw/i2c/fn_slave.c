/*
 * Fiscal storage (ФН) slave — Описание-ФН-1-2 frames over I2C.
 * Ported from PosCenterFiscalPrinterEmul FnSlave (factory-clean boot + E0).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/i2c/fn_slave.h"
#include "qemu/error-report.h"
#include <glib/gstdio.h>
#include <stddef.h>

static uint16_t fn_crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    size_t i;
    int b;

    for (i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

static const char *fn_cmd_name(uint8_t cmd)
{
    switch (cmd) {
    case 0x02: return "Начать фискализацию";
    case 0x03: return "Фискализация";
    case 0x04: return "Начать закрытие ФН";
    case 0x05: return "Закрыть фискальный режим";
    case 0x06: return "Отменить документ";
    case 0x07: return "Передать данные документа (TLV)";
    case 0x10: return "Запрос состояния смены";
    case 0x11: return "Начать открытие смены";
    case 0x12: return "Открыть смену";
    case 0x13: return "Начать закрытие смены";
    case 0x14: return "Закрыть смену";
    case 0x15: return "Начать чек";
    case 0x16: return "Сформировать чек";
    case 0x17: return "Начать формирование чека коррекции (БСО)";
    case 0x18: return "Начать отчёт о состоянии расчётов";
    case 0x19: return "Сформировать отчёт о состоянии расчётов";
    case 0x20: return "Получить статус информационного обмена";
    case 0x21: return "Передать статус транспортного соединения с ОФД";
    case 0x22: return "Начать чтение сообщения для ОФД";
    case 0x23: return "Прочитать блок сообщения для ОФД";
    case 0x24: return "Отменить чтение сообщения для ОФД";
    case 0x25: return "Завершить чтение сообщения для ОФД";
    case 0x26: return "Передать квитанцию от ОФД";
    case 0x30: return "Запрос статуса ФН";
    case 0x31: return "Запрос номера ФН";
    case 0x32: return "Запрос срока действия ФН";
    case 0x33: return "Запрос версии ФН";
    case 0x34: return "Запрос срока жизни ФН";
    case 0x35: return "Запрос последних ошибок ФН";
    case 0x36: return "Запрос счетчиков ФН";
    case 0x37: return "Запрос счетчиков операций ФН";
    case 0x38: return "Запрос счетчиков ФН по типу расчётов";
    case 0x39: return "Запрос счётчиков итогов непереданных документов";
    case 0x3A: return "Запрос формата ФН";
    case 0x3B: return "Запрос оставшегося срока действия ФН";
    case 0x3D: return "Запрос ресурса свободной памяти в ФН";
    case 0x3F: return "Запрос исполнения ФН";
    case 0x40: return "Найти фискальный документ по номеру";
    case 0x41: return "Запрос квитанции о получении ФД ОФД";
    case 0x42: return "Запрос количества ФД без квитанции";
    case 0x43: return "Запрос итогов открытия / регистрации ФН";
    case 0x44: return "Запрос параметра открытия ФН";
    case 0x45: return "Запрос фискального документа в TLV";
    case 0x46: return "Чтение TLV фискального документа";
    case 0x47: return "Чтение TLV параметров открытия ФН";
    case 0x50: return "Найти фискальный документ формата по номеру";
    case 0x60: return "Сброс состояния ФН";
    case 0xA2: return "Начать отчёт о регистрации (ФФД)";
    case 0xA3: return "Сформировать отчёт о регистрации (ФФД)";
    case 0xA7: return "Запрос размера данных, переданных 07h/B7h";
    case 0xAB: return "Переход на повышенную скорость UART";
    case 0xB0: return "Запрос статуса ФН по работе с КМ";
    case 0xB1: return "Передать код маркировки для проверки";
    case 0xB2: return "Сохранить результаты проверки КМ";
    case 0xB3: return "Очистить все результаты проверки КМ";
    case 0xB5: return "Сформировать запрос о коде маркировки";
    case 0xB6: return "Передать ответ на запрос о КМ";
    case 0xB7: return "Передать данные маркированных товаров";
    case 0xBA: return "Получить состояние по передаче уведомлений";
    case 0xBB: return "Начать чтение уведомления";
    case 0xBC: return "Прочитать блок данных уведомления";
    case 0xBD: return "Отменить чтение уведомления";
    case 0xBE: return "Завершить чтение уведомления";
    case 0xBF: return "Передать квитанцию на уведомление";
    case 0xD0: return "Получить запрос на обновление ключей проверки";
    case 0xD1: return "Передать ответ на запрос обновления ключей";
    case 0xD3: return "Начать сессию выгрузки уведомлений";
    case 0xD4: return "Следующее уведомление / параметры текущего";
    case 0xD5: return "Прочитать блок текущего уведомления";
    case 0xD6: return "Подтвердить выгрузку уведомления";
    case 0xD7: return "Получить адрес сервера обновления ключей";
    default:   return "неизвестная команда";
    }
}

static const char *fn_answer_name(uint8_t ans)
{
    switch (ans) {
    case 0x00: return "ошибок нет";
    case 0x01: return "неизвестная команда / неверный формат";
    case 0x02: return "другое состояние ФН";
    case 0x03: return "отказ ФН";
    case 0x04: return "отказ КС";
    case 0x05: return "параметры не соответствуют сроку жизни";
    case 0x07: return "неверные дата и/или время";
    case 0x08: return "нет запрошенных данных";
    case 0x09: return "некорректное значение параметров";
    case 0x0A: return "не все необходимые параметры / некорректная команда";
    case 0x0B: return "недопустимый реквизит в блоке данных";
    case 0x0C: return "дублирование реквизита";
    case 0x0D: return "отсутствуют необходимые данные";
    case 0x10: return "переполнение TLV";
    case 0x11: return "нет транспортного соединения";
    case 0x12: return "исчерпан ресурс ФН";
    case 0x14: return "ограничение ресурса ФН";
    case 0x16: return "продолжительность смены более 24 часов";
    case 0x20: return "сообщение/квитанция не может быть принята";
    case 0x32: return "запрещена работа с маркированными товарами";
    case 0x33: return "неверная последовательность команд КМ";
    case 0x34: return "исчерпан ресурс хранения документов для ОИСМ";
    case 0x35: return "переполнена таблица проверенных КМ";
    default:   return "код ФН";
    }
}

static const char *fn_doc_name(uint8_t doc)
{
    switch (doc) {
    case 0x00: return "нет";
    case 0x01: return "отчёт о фискализации";
    case 0x02: return "отчёт об открытии смены";
    case 0x04: return "кассовый чек";
    case 0x08: return "отчёт о закрытии смены";
    case 0x10: return "отчёт о закрытии ФН";
    case 0x11: return "БСО";
    case 0x12: return "перерегистрация (замена ФН)";
    case 0x13: return "перерегистрация";
    case 0x14: return "чек коррекции";
    case 0x15: return "БСО коррекции";
    case 0x17: return "отчёт о состоянии расчётов";
    default:   return "?";
    }
}

static uint16_t fn_u16le(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t fn_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void fn_fmt_hex(GString *g, const uint8_t *data, size_t len)
{
    size_t i, n = len;

    if (n > 512) {
        n = 512;
    }
    for (i = 0; i < n; i++) {
        if (i) {
            g_string_append_c(g, ' ');
        }
        g_string_append_printf(g, "%02X", data[i]);
    }
    if (len > n) {
        g_string_append_printf(g, " … (+%zu байт)", len - n);
    }
}

static void fn_fmt_ascii(GString *g, const uint8_t *p, size_t n)
{
    size_t i;

    g_string_append_c(g, '"');
    for (i = 0; i < n; i++) {
        char c = (char)p[i];
        if (c < 32 || c > 126) {
            g_string_append_printf(g, "\\x%02X", p[i]);
        } else {
            g_string_append_c(g, c);
        }
    }
    g_string_append_c(g, '"');
}

static void fn_fmt_dt(GString *g, const char *name, const uint8_t *p)
{
    g_string_append_printf(g, "%s=%02u.%02u.%02u %02u:%02u",
                           name, p[0], p[1], p[2], p[3], p[4]);
}

static void fn_fmt_life(GString *g, uint8_t v)
{
    bool first = true;

    g_string_append_printf(g, "фаза_жизни=0x%02X", v);
    if (v == 0) {
        g_string_append(g, " (заводской)");
        return;
    }
    g_string_append(g, " [");
#define FN_LIFE_BIT(b, name)                                                   \
    do {                                                                       \
        if (v & (1u << (b))) {                                                 \
            if (!first) {                                                      \
                g_string_append(g, ", ");                                      \
            }                                                                  \
            g_string_append(g, (name));                                        \
            first = false;                                                     \
        }                                                                      \
    } while (0)
    FN_LIFE_BIT(0, "настройка");
    FN_LIFE_BIT(1, "фискальный режим");
    FN_LIFE_BIT(2, "архив закрыт");
    FN_LIFE_BIT(3, "передача в ОФД закончена");
#undef FN_LIFE_BIT
    g_string_append_c(g, ']');
}

static void fn_fmt_tlv(GString *g, const uint8_t *p, size_t n)
{
    size_t i = 0;
    bool first = true;

    while (i + 4 <= n) {
        uint16_t tag = fn_u16le(p + i);
        uint16_t ln = fn_u16le(p + i + 2);

        i += 4;
        if (!first) {
            g_string_append(g, ", ");
        }
        first = false;
        g_string_append_printf(g, "TLV %04Xh len=%u", tag, ln);
        if (i + ln > n) {
            g_string_append(g, " (обрезан)");
            break;
        }
        if (ln) {
            g_string_append(g, "=");
            fn_fmt_ascii(g, p + i, ln);
            i += ln;
        }
    }
    if (first) {
        g_string_append(g, "данные=");
        fn_fmt_hex(g, p, n);
    }
}

static void fn_fmt_req_params(GString *g, uint8_t cmd,
                              const uint8_t *p, size_t n)
{
    if (!p || !n) {
        g_string_append(g, "—");
        return;
    }
    switch (cmd) {
    case 0x02:
        g_string_append_printf(g, "тип_отчёта=%u", p[0]);
        break;
    case 0xA2:
        g_string_append_printf(g, "тип_отчёта=%u", p[0]);
        if (n >= 2) {
            g_string_append_printf(g, ", ФФД=%u", p[1]);
        }
        break;
    case 0x03:
    case 0xA3:
        if (n >= 5) {
            fn_fmt_dt(g, "дата_время", p);
        }
        if (n >= 17) {
            g_string_append(g, ", ИНН=");
            fn_fmt_ascii(g, p + 5, 12);
        }
        if (n >= 37) {
            g_string_append(g, ", РНМ=");
            fn_fmt_ascii(g, p + 17, 20);
        }
        if (n >= 38) {
            g_string_append_printf(g, ", СНО=0x%02X", p[37]);
        }
        if (n >= 39) {
            g_string_append_printf(g, ", режим=0x%02X", p[38]);
        }
        if (n >= 40) {
            g_string_append_printf(g, ", расш=0x%02X", p[39]);
        }
        if (n >= 52) {
            g_string_append(g, ", ИНН_ОФД=");
            fn_fmt_ascii(g, p + 40, 12);
        }
        if (n >= 56) {
            g_string_append_printf(g, ", причина=0x%08X", fn_u32le(p + 52));
        }
        break;
    case 0x05:
        if (n >= 5) {
            fn_fmt_dt(g, "дата_время", p);
        }
        if (n >= 25) {
            g_string_append(g, ", РНМ=");
            fn_fmt_ascii(g, p + 5, 20);
        }
        break;
    case 0x07:
        fn_fmt_tlv(g, p, n);
        break;
    case 0x11:
    case 0x13:
    case 0x15:
    case 0x17:
        if (n >= 5) {
            fn_fmt_dt(g, "дата_время", p);
        }
        break;
    case 0x16:
        if (n >= 5) {
            fn_fmt_dt(g, "дата_время", p);
        }
        if (n >= 6) {
            g_string_append_printf(g, ", тип_операции=%u", p[5]);
        }
        if (n >= 10) {
            g_string_append_printf(g, ", сумма=%u", fn_u32le(p + 6));
        }
        break;
    case 0x18:
        if (n >= 5) {
            fn_fmt_dt(g, "дата_время", p);
        }
        break;
    case 0x19:
        g_string_append(g, "—");
        break;
    case 0x21:
        g_string_append_printf(g, "транспорт_ОФД=%u", p[0]);
        break;
    case 0x23:
    case 0xBC:
    case 0xD5:
        if (n >= 2) {
            g_string_append_printf(g, "смещение=%u", fn_u16le(p));
        }
        if (n >= 4) {
            g_string_append_printf(g, ", макс_длина=%u", fn_u16le(p + 2));
        }
        break;
    case 0x36:
    case 0x37:
        g_string_append_printf(g, "тип_счётчиков=%u (%s)", p[0],
                               p[0] ? "ФН" : "смена");
        break;
    case 0x38:
        g_string_append_printf(g, "тип_счётчиков=%u, тип_расчёта=%u",
                               p[0], n >= 2 ? p[1] : 0);
        break;
    case 0x3B:
        if (n >= 5) {
            fn_fmt_dt(g, "дата_время", p);
        }
        break;
    case 0x40:
    case 0x45:
    case 0x50:
        if (n >= 4) {
            g_string_append_printf(g, "номер_ФД=%u", fn_u32le(p));
        }
        break;
    case 0x43:
        if (n >= 1) {
            g_string_append_printf(g, "номер_отчёта=%u", p[0]);
        }
        break;
    case 0x44:
        if (n >= 3) {
            g_string_append_printf(g, "номер_отчёта=%u, тег=%04Xh",
                                   p[0], fn_u16le(p + 1));
        } else if (n >= 2) {
            g_string_append_printf(g, "тег=%04Xh", fn_u16le(p));
        }
        break;
    case 0x60:
        g_string_append_printf(g, "код=%u", p[0]);
        break;
    case 0xAB:
        if (n >= 4) {
            g_string_append_printf(g, "скорость=%u", fn_u32le(p));
        }
        break;
    case 0xB0:
        g_string_append_printf(g, "доп_код=%u", p[0]);
        break;
    case 0xB1:
        g_string_append_printf(g, "тип_КМ=%u, длина=%u", p[0],
                               n >= 2 ? p[1] : 0);
        break;
    case 0xB2:
        g_string_append_printf(g, "сохранить=%u", p[0]);
        break;
    case 0xB5:
        if (n >= 5) {
            fn_fmt_dt(g, "дата_время", p);
        }
        if (n > 5) {
            g_string_append(g, ", ");
            fn_fmt_tlv(g, p + 5, n - 5);
        }
        break;
    case 0xB7:
        g_string_append_printf(g, "доп_код=%u", p[0]);
        if (n > 1) {
            g_string_append(g, ", ");
            fn_fmt_tlv(g, p + 1, n - 1);
        }
        break;
    case 0xD0:
        if (n >= 5) {
            fn_fmt_dt(g, "дата_время", p);
        }
        break;
    case 0xD3:
    case 0xD4:
    case 0xD6:
        g_string_append_printf(g, "доп_код=%u", p[0]);
        break;
    case 0xD7:
        g_string_append_printf(g, "код_запроса=%u (%s)", p[0],
                               p[0] ? "URI" : "флаг обновления ключей");
        break;
    default:
        g_string_append(g, "данные=");
        fn_fmt_hex(g, p, n);
        break;
    }
    if (g->len == 0) {
        g_string_append(g, "данные=");
        fn_fmt_hex(g, p, n);
    }
}

static void fn_fmt_rsp_params(GString *g, uint8_t cmd, uint8_t ans,
                              const uint8_t *p, size_t n)
{
    if (ans != 0x00) {
        if (n) {
            g_string_append(g, "данные=");
            fn_fmt_hex(g, p, n);
        } else {
            g_string_append(g, "—");
        }
        return;
    }
    if (!p || !n) {
        g_string_append(g, "—");
        return;
    }
    switch (cmd) {
    case 0x30:
        if (n >= 30) {
            fn_fmt_life(g, p[0]);
            g_string_append_printf(g, ", документ=%02Xh (%s), данные_дока=%u, смена=%s",
                                   p[1], fn_doc_name(p[1]), p[2],
                                   p[3] ? "открыта" : "закрыта");
            g_string_append(g, ", ");
            fn_fmt_dt(g, "дата_время", p + 5);
            g_string_append(g, ", СН=");
            fn_fmt_ascii(g, p + 10, 16);
            g_string_append_printf(g, ", last_ФД=%u", fn_u32le(p + 26));
        }
        break;
    case 0x31:
        g_string_append(g, "СН=");
        fn_fmt_ascii(g, p, n < 16 ? n : 16);
        break;
    case 0x32:
        if (n >= 3) {
            g_string_append_printf(g, "срок=%02u.%02u.%02u", p[0], p[1], p[2]);
        }
        break;
    case 0x33:
        g_string_append(g, "версия=");
        fn_fmt_ascii(g, p, n >= 16 ? 16 : n);
        if (n >= 17) {
            g_string_append_printf(g, ", тип=%u (%s)", p[16],
                                   p[16] ? "серийная" : "отладочная");
        }
        break;
    case 0x3A:
        if (n >= 2) {
            g_string_append_printf(g, "ФФД=%u, макс_ФФД=%u", p[0], p[1]);
        }
        break;
    case 0x3B:
        if (n >= 2) {
            g_string_append_printf(g, "осталось_дней=%u", fn_u16le(p));
        }
        break;
    case 0x3D:
        if (n >= 8) {
            g_string_append_printf(g, "доков_5лет=%u, память_30дн_КиБ=%u",
                                   fn_u32le(p), fn_u32le(p + 4));
        }
        if (n >= 9) {
            g_string_append_printf(g, ", уведомления=%u%%", p[8]);
        }
        break;
    case 0x3F:
        g_string_append(g, "исполнение=");
        fn_fmt_ascii(g, p, n);
        break;
    case 0x03:
    case 0x05:
    case 0xA3:
        if (n >= 8) {
            g_string_append_printf(g, "номер_ФД=%u, ФП=%08X",
                                   fn_u32le(p), fn_u32le(p + 4));
        }
        break;
    case 0x19:
        if (n >= 12) {
            g_string_append_printf(g, "номер_ФД=%u, ФП=%08X, неподтверждённых=%u",
                                   fn_u32le(p), fn_u32le(p + 4), fn_u32le(p + 8));
        }
        break;
    case 0x10:
        if (n >= 5) {
            g_string_append_printf(g, "смена=%s, номер_смены=%u, чеков=%u",
                                   p[0] ? "открыта" : "закрыта",
                                   fn_u16le(p + 1), fn_u16le(p + 3));
        }
        break;
    case 0x12:
    case 0x14:
    case 0x16:
        if (n >= 10) {
            g_string_append_printf(g, "номер_смены=%u, номер_ФД=%u, ФП=%08X",
                                   fn_u16le(p), fn_u32le(p + 2), fn_u32le(p + 6));
        }
        break;
    case 0x20:
        if (n >= 2) {
            g_string_append_printf(g, "транспорт=%u, чтение=%u", p[0], p[1]);
        }
        break;
    case 0x22:
        if (n >= 2) {
            g_string_append_printf(g, "длина_сообщения=%u", fn_u16le(p));
        }
        break;
    case 0x23:
        if (n >= 2) {
            g_string_append_printf(g, "длина=%u", fn_u16le(p));
            if (n > 2) {
                g_string_append(g, ", данные=");
                fn_fmt_hex(g, p + 2, n - 2);
            }
        }
        break;
    case 0x40:
        if (n >= 10) {
            g_string_append_printf(g,
                                   "тип=%02Xh (%s), ОФД_ack=%u, номер_ФД=%u, ФП=%08X",
                                   p[0], fn_doc_name(p[0]), p[1],
                                   fn_u32le(p + 2), fn_u32le(p + 6));
        }
        break;
    case 0x42:
        if (n >= 2) {
            g_string_append_printf(g, "в_очереди_ОФД=%u", fn_u16le(p));
        }
        break;
    case 0x43:
        if (n >= 47) {
            unsigned o = 37;

            fn_fmt_dt(g, "дата_время", p);
            g_string_append(g, ", ИНН=");
            fn_fmt_ascii(g, p + 5, 12);
            g_string_append(g, ", РНМ=");
            fn_fmt_ascii(g, p + 17, 20);
            g_string_append_printf(g, ", СНО=0x%02X, режим=0x%02X", p[37], p[38]);
            o = 39;
            if (n >= 60) {
                g_string_append_printf(g, ", расш=0x%02X, ИНН_ОФД=", p[39]);
                fn_fmt_ascii(g, p + 40, 12);
                o = 52;
            }
            if (n >= o + 12) {
                g_string_append_printf(g, ", причина=%u", fn_u32le(p + o));
                o += 4;
            }
            if (n >= o + 8) {
                g_string_append_printf(g, ", last_ФД=%u, ФП=%08X",
                                       fn_u32le(p + o), fn_u32le(p + o + 4));
            }
        }
        break;
    case 0x44:
        fn_fmt_tlv(g, p, n);
        break;
    case 0x45:
        if (n >= 4) {
            g_string_append_printf(g, "тип_ФД=%04Xh, длина=%u",
                                   fn_u16le(p), fn_u16le(p + 2));
        }
        break;
    case 0xA7:
        if (n >= 8) {
            g_string_append_printf(g, "размер_ОФД=%u, размер_ОИСМ=%u",
                                   fn_u32le(p), fn_u32le(p + 4));
        }
        break;
    case 0xB0:
        if (n == 1) {
            g_string_append_printf(g, "разрешение_без_запроса_КМ=%u", p[0]);
        } else if (n >= 9) {
            g_string_append_printf(g,
                                   "проверка_КМ=%u, уведомление=%u, флаги=0x%02X, "
                                   "сохранено_КМ=%u, КМ_в_уведомлении=%u, "
                                   "заполнение=%u, очередь_уведомлений=%u",
                                   p[0], p[1], p[2], p[3], p[4], p[5],
                                   fn_u16le(p + 6));
        }
        break;
    case 0xB1:
        if (n >= 2) {
            g_string_append_printf(g, "результат=%u, причина=%u", p[0], p[1]);
        }
        break;
    case 0xB2:
        g_string_append_printf(g, "результат_проверки=%u", p[0]);
        break;
    case 0xBA:
        if (n >= 13) {
            g_string_append_printf(g,
                                   "состояние=%u, очередь=%u, номер=%u, заполнение=%u%%",
                                   p[0], fn_u16le(p + 1), fn_u32le(p + 3), p[12]);
        }
        break;
    case 0xBB:
        if (n >= 2) {
            g_string_append_printf(g, "длина_уведомления=%u", fn_u16le(p));
        }
        break;
    case 0xD3:
        if (n >= 12) {
            g_string_append_printf(g, "неподтверждённых=%u, номер=%u, в_сессии=%u",
                                   fn_u16le(p), fn_u32le(p + 2), fn_u16le(p + 6));
        }
        break;
    case 0xD7:
        if (n == 1) {
            g_string_append_printf(g, "нужно_обновление_ключей=%u", p[0]);
        } else {
            g_string_append(g, "URI=");
            fn_fmt_ascii(g, p, n);
        }
        break;
    default:
        g_string_append(g, "данные=");
        fn_fmt_hex(g, p, n);
        break;
    }
    if (g->len == 0) {
        g_string_append(g, "данные=");
        fn_fmt_hex(g, p, n);
    }
}

static void fn_log_open(FnSlave *fn)
{
    g_autofree char *dir = NULL;
    GDateTime *dt;

    if (fn->log || fn->log_open_failed || !fn->log_path || !fn->log_path[0]) {
        return;
    }
    dir = g_path_get_dirname(fn->log_path);
    if (dir && dir[0] && strcmp(dir, ".") != 0) {
        g_mkdir_with_parents(dir, 0755);
    }
    fn->log = fopen(fn->log_path, "a");
    if (!fn->log) {
        fn->log_open_failed = true;
        error_report("fn-slave: cannot open log %s", fn->log_path);
        return;
    }
    dt = g_date_time_new_now_local();
    {
        char ts[40];
        int ms = g_date_time_get_microsecond(dt) / 1000;

        g_snprintf(ts, sizeof(ts), "[%02d.%02d.%04d %02d:%02d:%02d.%03d]",
                   g_date_time_get_day_of_month(dt),
                   g_date_time_get_month(dt),
                   g_date_time_get_year(dt),
                   g_date_time_get_hour(dt),
                   g_date_time_get_minute(dt),
                   g_date_time_get_second(dt),
                   ms);
        fprintf(fn->log,
                "%s ------------------------------------------------------------\n"
                "%s ФН       : fn 1.2 mgm 03\n"
                "%s SN       : %.16s\n"
                "%s ------------------------------------------------------------\n",
                ts, ts, ts, fn->sn[0] ? fn->sn : "—", ts);
    }
    g_date_time_unref(dt);
    fflush(fn->log);
    info_report("fn-slave: log %s", fn->log_path);
}

void fn_slave_set_log_path(FnSlave *fn, const char *path)
{
    g_free(fn->log_path);
    fn->log_path = (path && path[0]) ? g_strdup(path) : NULL;
    fn->log_open_failed = false;
}

void fn_slave_close_log(FnSlave *fn)
{
    if (fn->log) {
        fclose(fn->log);
        fn->log = NULL;
    }
    g_free(fn->log_path);
    fn->log_path = NULL;
}

static void fn_reg_apply_mode_tags(FnSlave *fn);

#define FN_NV_MAGIC   0x314E4651u /* QFN1 */
#define FN_NV_VERSION 2

typedef struct QEMU_PACKED FnNvDoc {
    uint8_t type;
    uint8_t ofd_ack;
    uint32_t fd_no;
    uint32_t fp;
    uint16_t shift_no;
    uint8_t op_type;
    uint8_t pad;
    uint32_t amount_kop;
} FnNvDoc;

typedef struct QEMU_PACKED FnNvFile {
    uint32_t magic;
    uint32_t version;
    char sn[17];
    char inn[13];
    char rnm[21];
    uint8_t tax_system;
    uint8_t work_mode;
    uint8_t ffd_version;
    uint8_t last_dt[5];
    uint8_t fiscal_open;
    uint8_t shift_open;
    uint8_t life_phase;
    uint8_t marking;
    uint32_t last_fd_no;
    uint32_t shift_number;
    uint32_t receipt_in_shift;
    uint32_t last_fp;
    uint32_t archive_n;
    uint32_t ofd_qn;
    FnNvDoc archive[FN_ARCHIVE_MAX];
    uint32_t ofd_queue[FN_ARCHIVE_MAX];
    char ofd_inn[13];
    uint8_t ext_flags;
    uint8_t pad_v2[3];
    uint32_t reason_code;
    uint32_t reg_tlv_len;
    uint8_t reg_tlv[FN_TLV_MAX];
} FnNvFile;

static void fn_nv_save(FnSlave *fn)
{
    FnNvFile rec;
    g_autofree char *tmp = NULL;
    FILE *f;
    unsigned i;

    if (!fn->persist_path || !fn->persist_path[0]) {
        return;
    }
    memset(&rec, 0, sizeof(rec));
    rec.magic = FN_NV_MAGIC;
    rec.version = FN_NV_VERSION;
    memcpy(rec.sn, fn->sn, sizeof(rec.sn));
    memcpy(rec.inn, fn->inn, sizeof(rec.inn));
    memcpy(rec.rnm, fn->rnm, sizeof(rec.rnm));
    rec.tax_system = fn->tax_system;
    rec.work_mode = fn->work_mode;
    rec.ffd_version = fn->ffd_version;
    memcpy(rec.last_dt, fn->last_dt, 5);
    rec.fiscal_open = fn->fiscal_open ? 1 : 0;
    rec.shift_open = fn->shift_open ? 1 : 0;
    rec.life_phase = fn->life_phase;
    rec.marking = fn->marking ? 1 : 0;
    rec.last_fd_no = fn->last_fd_no;
    rec.shift_number = fn->shift_number;
    rec.receipt_in_shift = fn->receipt_in_shift;
    rec.last_fp = fn->last_fp;
    rec.archive_n = fn->archive_n;
    rec.ofd_qn = fn->ofd_qn;
    memcpy(rec.ofd_inn, fn->ofd_inn, sizeof(rec.ofd_inn));
    rec.ext_flags = fn->ext_flags;
    rec.reason_code = fn->reason_code;
    rec.reg_tlv_len = fn->reg_tlv_len;
    if (rec.reg_tlv_len > FN_TLV_MAX) {
        rec.reg_tlv_len = FN_TLV_MAX;
    }
    memcpy(rec.reg_tlv, fn->reg_tlv, rec.reg_tlv_len);
    for (i = 0; i < fn->archive_n && i < FN_ARCHIVE_MAX; i++) {
        rec.archive[i].type = fn->archive[i].type;
        rec.archive[i].ofd_ack = fn->archive[i].ofd_ack;
        rec.archive[i].fd_no = fn->archive[i].fd_no;
        rec.archive[i].fp = fn->archive[i].fp;
        rec.archive[i].shift_no = fn->archive[i].shift_no;
        rec.archive[i].op_type = fn->archive[i].op_type;
        rec.archive[i].amount_kop = fn->archive[i].amount_kop;
    }
    memcpy(rec.ofd_queue, fn->ofd_queue, sizeof(rec.ofd_queue));

    tmp = g_strdup_printf("%s.tmp", fn->persist_path);
    f = fopen(tmp, "wb");
    if (!f) {
        error_report("fn-slave: cannot write %s", tmp);
        return;
    }
    if (fwrite(&rec, sizeof(rec), 1, f) != 1) {
        fclose(f);
        unlink(tmp);
        error_report("fn-slave: cannot write %s", tmp);
        return;
    }
    fclose(f);
    unlink(fn->persist_path);
    if (rename(tmp, fn->persist_path) != 0) {
        unlink(tmp);
        error_report("fn-slave: cannot replace %s", fn->persist_path);
    }
}

static bool fn_nv_load(FnSlave *fn)
{
    FnNvFile rec;
    FILE *f;
    unsigned i, n;

    if (!fn->persist_path || !fn->persist_path[0]) {
        return false;
    }
    f = fopen(fn->persist_path, "rb");
    if (!f) {
        return false;
    }
    memset(&rec, 0, sizeof(rec));
    if (fread(&rec, 1, sizeof(rec), f) < offsetof(FnNvFile, ofd_inn)) {
        fclose(f);
        return false;
    }
    fclose(f);
    if (rec.magic != FN_NV_MAGIC ||
        (rec.version != 1 && rec.version != FN_NV_VERSION)) {
        error_report("fn-slave: ignore %s (magic/version)", fn->persist_path);
        return false;
    }
    memcpy(fn->sn, rec.sn, sizeof(fn->sn));
    fn->sn[16] = 0;
    memcpy(fn->inn, rec.inn, sizeof(fn->inn));
    fn->inn[12] = 0;
    memcpy(fn->rnm, rec.rnm, sizeof(fn->rnm));
    fn->rnm[20] = 0;
    fn->tax_system = rec.tax_system;
    fn->work_mode = rec.work_mode;
    fn->ffd_version = rec.ffd_version;
    memcpy(fn->last_dt, rec.last_dt, 5);
    fn->fiscal_open = rec.fiscal_open != 0;
    fn->shift_open = rec.shift_open != 0;
    fn->life_phase = rec.life_phase;
    fn->marking = rec.marking != 0;
    fn->last_fd_no = rec.last_fd_no;
    fn->shift_number = rec.shift_number;
    fn->receipt_in_shift = rec.receipt_in_shift;
    fn->last_fp = rec.last_fp;
    n = rec.archive_n;
    if (n > FN_ARCHIVE_MAX) {
        n = FN_ARCHIVE_MAX;
    }
    fn->archive_n = n;
    for (i = 0; i < n; i++) {
        fn->archive[i].type = rec.archive[i].type;
        fn->archive[i].ofd_ack = rec.archive[i].ofd_ack;
        fn->archive[i].fd_no = rec.archive[i].fd_no;
        fn->archive[i].fp = rec.archive[i].fp;
        fn->archive[i].shift_no = rec.archive[i].shift_no;
        fn->archive[i].op_type = rec.archive[i].op_type;
        fn->archive[i].amount_kop = rec.archive[i].amount_kop;
    }
    fn->ofd_qn = rec.ofd_qn;
    if (fn->ofd_qn > FN_ARCHIVE_MAX) {
        fn->ofd_qn = FN_ARCHIVE_MAX;
    }
    memcpy(fn->ofd_queue, rec.ofd_queue, sizeof(fn->ofd_queue));
    if (rec.version >= 2) {
        memcpy(fn->ofd_inn, rec.ofd_inn, sizeof(fn->ofd_inn));
        fn->ofd_inn[12] = 0;
        fn->ext_flags = rec.ext_flags;
        fn->reason_code = rec.reason_code;
        fn->reg_tlv_len = rec.reg_tlv_len;
        if (fn->reg_tlv_len > FN_TLV_MAX) {
            fn->reg_tlv_len = FN_TLV_MAX;
        }
        memcpy(fn->reg_tlv, rec.reg_tlv, fn->reg_tlv_len);
        fn_reg_apply_mode_tags(fn);
    } else {
        memset(fn->ofd_inn, ' ', 12);
        fn->ofd_inn[12] = 0;
        fn->ext_flags = 0;
        fn->reason_code = 0;
        fn->reg_tlv_len = 0;
        fn_reg_apply_mode_tags(fn);
    }
    info_report("fn-slave: persist %s (phase=0x%02X shift=%s fd=%u)",
                fn->persist_path, fn->life_phase,
                fn->shift_open ? "open" : "closed", fn->last_fd_no);
    return true;
}

void fn_slave_set_persist_path(FnSlave *fn, const char *path)
{
    g_free(fn->persist_path);
    fn->persist_path = (path && path[0]) ? g_strdup(path) : NULL;
    if (fn->persist_path && !fn_nv_load(fn)) {
        info_report("fn-slave: no persist file, factory-clean FN");
    }
}

void fn_slave_flush(FnSlave *fn)
{
    fn_nv_save(fn);
}

static void fn_log_line(FnSlave *fn, bool to_fn, uint8_t cmd,
                        const uint8_t *frame, size_t frame_len,
                        uint8_t answer, const uint8_t *payload, size_t plen,
                        bool malformed, const char *why)
{
    GDateTime *dt;
    GString *hex = g_string_new(NULL);
    GString *params = g_string_new(NULL);
    int ms;
    char ts[40];
    const char *lvl;

    fn_log_open(fn);
    if (!fn->log) {
        g_string_free(hex, TRUE);
        g_string_free(params, TRUE);
        return;
    }
    dt = g_date_time_new_now_local();
    ms = g_date_time_get_microsecond(dt) / 1000;
    g_snprintf(ts, sizeof(ts), "[%02d.%02d.%04d %02d:%02d:%02d.%03d]",
               g_date_time_get_day_of_month(dt),
               g_date_time_get_month(dt),
               g_date_time_get_year(dt),
               g_date_time_get_hour(dt),
               g_date_time_get_minute(dt),
               g_date_time_get_second(dt),
               ms);
    g_date_time_unref(dt);

    fn_fmt_hex(hex, frame, frame_len);
    lvl = (malformed || (!to_fn && answer != 0)) ? "ERROR" : "DEBUG";

    if (malformed) {
        fprintf(fn->log,
                "%s [%s] %s кадр не разобран\n"
                "%s [%s]   hex: %s\n"
                "%s [%s]   параметры: %s\n",
                ts, lvl, to_fn ? "->" : "<-",
                ts, lvl, hex->str,
                ts, lvl, why ? why : "—");
        fflush(fn->log);
        g_string_free(hex, TRUE);
        g_string_free(params, TRUE);
        return;
    }

    if (to_fn) {
        fn_fmt_req_params(params, cmd, payload, plen);
        fprintf(fn->log,
                "%s [%s] -> %02Xh %s\n"
                "%s [%s]   hex: %s\n"
                "%s [%s]   параметры: %s\n",
                ts, lvl, cmd, fn_cmd_name(cmd),
                ts, lvl, hex->str,
                ts, lvl, params->str);
    } else {
        fn_fmt_rsp_params(params, cmd, answer, payload, plen);
        fprintf(fn->log,
                "%s [%s] <- %02Xh %s\n"
                "%s [%s]   hex: %s\n"
                "%s [%s]   ответ: %02Xh (%s)\n"
                "%s [%s]   параметры: %s\n",
                ts, lvl, cmd, fn_cmd_name(cmd),
                ts, lvl, hex->str,
                ts, lvl, answer, fn_answer_name(answer),
                ts, lvl, params->str);
    }
    fflush(fn->log);
    g_string_free(hex, TRUE);
    g_string_free(params, TRUE);
}

static void fn_factory_clean(FnSlave *fn)
{
    static const uint8_t dt[5] = {26, 8, 12, 14, 0};

    memcpy(fn->sn, "9999078900012345", 16);
    fn->sn[16] = 0;
    memcpy(fn->inn, "770123456789", 12);
    fn->inn[12] = 0;
    memset(fn->rnm, '0', 20);
    fn->rnm[20] = 0;
    memset(fn->ofd_inn, ' ', 12);
    fn->ofd_inn[12] = 0;
    fn->tax_system = 0x01;
    fn->work_mode = 0x01;
    fn->ext_flags = 0;
    fn->ffd_version = 4;
    fn->reason_code = 0;
    memcpy(fn->last_dt, dt, 5);
    fn->fiscal_open = false;
    fn->shift_open = false;
    fn->life_phase = 0x00;
    fn->cur_doc = 0;
    fn->doc_data = 0;
    fn->ofd_transport = 0;
    fn->ofd_reading = 0;
    fn->last_fd_no = 0;
    fn->shift_number = 0;
    fn->receipt_in_shift = 0;
    fn->last_fp = 0xA5A5A5A5u;
    fn->doc_tlv_len = 0;
    fn->reg_tlv_len = 0;
    fn->archive_n = 0;
    fn->ofd_qn = 0;
    fn->ofd_read_off = 0;
    fn->ofd_msg_len = 0;
    fn->marking = true;
    fn->km_phase = 1;
    fn->km_saved = 0;
    fn->notif_xfer = 0;
    fn->tlv_read_kind = 0;
}

void fn_slave_cold_init(FnSlave *fn)
{
    memset(fn, 0, sizeof(*fn));
    fn_factory_clean(fn);
}

void fn_slave_reset(FnSlave *fn)
{
    /* I2C / KKT reset: cancel the open document, keep FN flash. */
    fn->cur_doc = 0;
    fn->doc_data = 0;
    fn->doc_tlv_len = 0;
    fn->ofd_transport = 0;
    fn->ofd_reading = 0;
    fn->ofd_read_off = 0;
    fn->ofd_msg_len = 0;
    fn->km_phase = 1;
    fn->km_saved = 0;
    fn->notif_xfer = 0;
    fn->tlv_read_kind = 0;
    fn->tlv_read_off = 0;
    fn->rx_len = 0;
    fn->rx_pos = 0;
    fn->busy_nack_left = 0;
    fn->last_cmd = 0;
}

bool fn_slave_busy(const FnSlave *fn)
{
    return fn->busy_nack_left > 0;
}

void fn_slave_note_address_probe(FnSlave *fn, bool is_read)
{
    (void)is_read;
    if (fn->busy_nack_left > 0) {
        fn->busy_nack_left--;
    }
}

bool fn_slave_pop_rx(FnSlave *fn, uint8_t *out)
{
    if (fn->rx_pos >= fn->rx_len) {
        return false;
    }
    *out = fn->rx[fn->rx_pos++];
    return true;
}

static void fn_emit_frame(FnSlave *fn, uint8_t answer,
                          const uint8_t *data, size_t len)
{
    uint16_t body_len = (uint16_t)(len + 1u);
    uint16_t crc;
    size_t n = 0;

    if (4 + len + 2 > FN_RX_MAX) {
        len = FN_RX_MAX - 6;
        body_len = (uint16_t)(len + 1u);
    }

    fn->rx[n++] = 0x04;
    fn->rx[n++] = body_len & 0xFF;
    fn->rx[n++] = (body_len >> 8) & 0xFF;
    fn->rx[n++] = answer;
    if (data && len) {
        memcpy(fn->rx + n, data, len);
        n += len;
    }
    crc = fn_crc16_ccitt(fn->rx + 1, n - 1);
    fn->rx[n++] = crc & 0xFF;
    fn->rx[n++] = (crc >> 8) & 0xFF;
    fn->rx_len = n;
    fn->rx_pos = 0;
    fn->busy_nack_left = 1;
}

static void fn_fill_status(const FnSlave *fn, uint8_t *st)
{
    memset(st, 0, 30);
    st[0] = fn->life_phase;
    st[1] = fn->cur_doc;
    st[2] = fn->doc_data;
    st[3] = fn->shift_open ? 0x01 : 0x00;
    memcpy(st + 5, fn->last_dt, 5);
    memcpy(st + 10, fn->sn, 16);
    st[26] = fn->last_fd_no & 0xFF;
    st[27] = (fn->last_fd_no >> 8) & 0xFF;
    st[28] = (fn->last_fd_no >> 16) & 0xFF;
    st[29] = (fn->last_fd_no >> 24) & 0xFF;
}

static void fn_store_dt(FnSlave *fn, const uint8_t *p, size_t n)
{
    if (p && n >= 5) {
        memcpy(fn->last_dt, p, 5);
    }
}

static void fn_begin_doc(FnSlave *fn, uint8_t kind)
{
    fn->cur_doc = kind;
    fn->doc_data = 0;
    fn->doc_tlv_len = 0;
}

static void fn_clear_doc(FnSlave *fn)
{
    fn->cur_doc = 0;
    fn->doc_data = 0;
    fn->doc_tlv_len = 0;
}

static uint32_t fn_next_fp(FnSlave *fn)
{
    fn->last_fp = fn->last_fp * 1664525u + 1013904223u;
    if (fn->last_fp == 0) {
        fn->last_fp = 1;
    }
    return fn->last_fp;
}

static void fn_archive_doc(FnSlave *fn, uint8_t type, uint8_t op, uint32_t amount)
{
    FnArchivedDoc *d;

    if (fn->archive_n >= FN_ARCHIVE_MAX) {
        memmove(fn->archive, fn->archive + 1,
                sizeof(fn->archive[0]) * (FN_ARCHIVE_MAX - 1));
        fn->archive_n = FN_ARCHIVE_MAX - 1;
    }
    d = &fn->archive[fn->archive_n++];
    memset(d, 0, sizeof(*d));
    d->type = type;
    d->fd_no = fn->last_fd_no;
    d->fp = fn->last_fp;
    d->shift_no = (uint16_t)fn->shift_number;
    d->op_type = op;
    d->amount_kop = amount;
    if (fn->ofd_qn < FN_ARCHIVE_MAX) {
        fn->ofd_queue[fn->ofd_qn++] = d->fd_no;
    }
    fn_nv_save(fn);
}

static FnArchivedDoc *fn_find_doc(FnSlave *fn, uint32_t fd)
{
    unsigned i;

    for (i = 0; i < fn->archive_n; i++) {
        if (fn->archive[i].fd_no == fd) {
            return &fn->archive[i];
        }
    }
    return NULL;
}

static void fn_emit_fd_fp(FnSlave *fn)
{
    uint8_t out[8];

    memcpy(out, &fn->last_fd_no, 4);
    memcpy(out + 4, &fn->last_fp, 4);
    fn_emit_frame(fn, 0x00, out, sizeof(out));
}

static bool fn_need_fiscal(FnSlave *fn)
{
    if ((fn->life_phase & 0x02) == 0) {
        fn_emit_frame(fn, 0x02, NULL, 0);
        return false;
    }
    return true;
}

static bool fn_need_marking(FnSlave *fn)
{
    if (!fn->marking) {
        fn_emit_frame(fn, 0x32, NULL, 0);
        return false;
    }
    return true;
}

static void fn_emit_zeros(FnSlave *fn, size_t n)
{
    uint8_t buf[384];

    if (n > sizeof(buf)) {
        n = sizeof(buf);
    }
    memset(buf, 0, n);
    fn_emit_frame(fn, 0x00, buf, n);
}

static void fn_emit_protected_stub(FnSlave *fn, size_t inner)
{
    uint8_t buf[32];
    uint16_t crc, len;

    if (inner < 4) {
        inner = 4;
    }
    if (inner > sizeof(buf)) {
        inner = sizeof(buf);
    }
    memset(buf, 0, inner);
    len = (uint16_t)inner;
    buf[0] = len & 0xFF;
    buf[1] = (len >> 8) & 0xFF;
    crc = fn_crc16_ccitt(buf, inner);
    buf[2] = crc & 0xFF;
    buf[3] = (crc >> 8) & 0xFF;
    fn_emit_frame(fn, 0x00, buf, inner);
}

static void fn_push_tlv(uint8_t *buf, unsigned *len, unsigned cap,
                        uint16_t tag, const uint8_t *v, uint16_t n)
{
    if (*len + 4 + n > cap) {
        return;
    }
    buf[(*len)++] = tag & 0xFF;
    buf[(*len)++] = (tag >> 8) & 0xFF;
    buf[(*len)++] = n & 0xFF;
    buf[(*len)++] = (n >> 8) & 0xFF;
    if (v && n) {
        memcpy(buf + *len, v, n);
        *len += n;
    }
}

static bool fn_is_reg_doc(uint8_t doc)
{
    return doc == 0x01 || doc == 0x12 || doc == 0x13;
}

static bool fn_tlv_find(const uint8_t *buf, unsigned len, uint16_t tag,
                        unsigned *off, unsigned *full_len)
{
    unsigned i = 0;

    while (i + 4 <= len) {
        uint16_t t = (uint16_t)(buf[i] | (buf[i + 1] << 8));
        uint16_t n = (uint16_t)(buf[i + 2] | (buf[i + 3] << 8));
        unsigned full = 4u + n;

        if (i + full > len) {
            break;
        }
        if (t == tag) {
            if (off) {
                *off = i;
            }
            if (full_len) {
                *full_len = full;
            }
            return true;
        }
        i += full;
    }
    return false;
}

static bool fn_tlv_at(const uint8_t *buf, unsigned len, unsigned off,
                      unsigned *full_len)
{
    uint16_t n;

    if (off + 4 > len) {
        return false;
    }
    n = (uint16_t)(buf[off + 2] | (buf[off + 3] << 8));
    if (off + 4u + n > len) {
        return false;
    }
    if (full_len) {
        *full_len = 4u + n;
    }
    return true;
}

static bool fn_ascii_has_digit(const char *s, size_t n)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (s[i] >= '0' && s[i] <= '9') {
            return true;
        }
    }
    return false;
}

static void fn_reg_upsert(FnSlave *fn, uint16_t tag, const uint8_t *v, uint16_t n)
{
    unsigned off = 0, full = 0;

    if (fn_tlv_find(fn->reg_tlv, fn->reg_tlv_len, tag, &off, &full)) {
        memmove(fn->reg_tlv + off, fn->reg_tlv + off + full,
                fn->reg_tlv_len - off - full);
        fn->reg_tlv_len -= full;
    }
    fn_push_tlv(fn->reg_tlv, &fn->reg_tlv_len, FN_TLV_MAX, tag, v, n);
}

static void fn_reg_delete(FnSlave *fn, uint16_t tag)
{
    unsigned off = 0, full = 0;

    if (!fn_tlv_find(fn->reg_tlv, fn->reg_tlv_len, tag, &off, &full)) {
        return;
    }
    memmove(fn->reg_tlv + off, fn->reg_tlv + off + full,
            fn->reg_tlv_len - off - full);
    fn->reg_tlv_len -= full;
}

static bool fn_tlv_payload_present(const uint8_t *v, unsigned n)
{
    unsigned i;

    for (i = 0; i < n; i++) {
        if (v[i] != 0 && v[i] != ' ') {
            return true;
        }
    }
    return false;
}

/* FFD 1.2 tag 1290 from work_mode (table 38) + ext_flags (table 51). */
static uint32_t fn_reg_pack_1290(const FnSlave *fn)
{
    uint32_t v = 0;

    if (fn->work_mode & 0x08) {
        v |= (1u << 9);  /* услуги */
    }
    if (fn->work_mode & 0x10) {
        v |= (1u << 2);  /* БСО */
    }
    if (fn->work_mode & 0x20) {
        v |= (1u << 5);  /* интернет */
    }
    if (fn->work_mode & 0x40) {
        v |= (1u << 15); /* общепит */
    }
    if (fn->work_mode & 0x80) {
        v |= (1u << 16); /* оптовая торговля */
    }
    if (fn->ext_flags & 0x01) {
        v |= (1u << 6);  /* подакцизный товар */
    }
    if (fn->ext_flags & 0x02) {
        v |= (1u << 10); /* азартные игры */
    }
    if (fn->ext_flags & 0x04) {
        v |= (1u << 11); /* лотерея */
    }
    if (fn->ext_flags & 0x08) {
        v |= (1u << 1);  /* принтер в автомате */
    }
    if (fn->ext_flags & 0x10) {
        v |= (1u << 8);  /* маркировка */
    }
    if (fn->ext_flags & 0x20) {
        v |= (1u << 12); /* ломбард */
    }
    if (fn->ext_flags & 0x40) {
        v |= (1u << 13); /* страхование */
    }
    if (fn->ext_flags & 0x80) {
        v |= (1u << 14); /* ККТ в автомате */
    }
    return v;
}

/*
 * Tags the FN itself forms from A3 work_mode / ext_flags. KKT 07h must not
 * supply them; leftover blanks (especially 1036 «номер автомата») make the
 * guest treat a cashier KKT as a vending automaton and reject FF45 with 53.
 */
static bool fn_reg_fill_mode_tag(const FnSlave *fn, uint16_t tag,
                                 uint8_t *tlv, unsigned *tlv_len, unsigned cap)
{
    uint8_t one = 1;
    uint32_t t1290;

    *tlv_len = 0;
    switch (tag) {
    case 0x03E9: /* 1001 автоматический режим */
        if ((fn->work_mode & 0x04) == 0) {
            return false;
        }
        fn_push_tlv(tlv, tlv_len, cap, tag, &one, 1);
        return true;
    case 0x03EA: /* 1002 автономный режим */
        if ((fn->work_mode & 0x02) == 0) {
            return false;
        }
        fn_push_tlv(tlv, tlv_len, cap, tag, &one, 1);
        return true;
    case 0x0420: /* 1056 шифрование */
        if ((fn->work_mode & 0x01) == 0) {
            return false;
        }
        fn_push_tlv(tlv, tlv_len, cap, tag, &one, 1);
        return true;
    case 0x04C5: /* 1221 принтер в автомате — только ФФД 1.1 */
        if (fn->ffd_version >= 4 || (fn->ext_flags & 0x08) == 0) {
            return false;
        }
        fn_push_tlv(tlv, tlv_len, cap, tag, &one, 1);
        return true;
    case 0x050A: /* 1290 признаки условий применения, ФФД 1.2 */
        if (fn->ffd_version < 4) {
            return false;
        }
        t1290 = fn_reg_pack_1290(fn);
        fn_push_tlv(tlv, tlv_len, cap, tag, (const uint8_t *)&t1290, 4);
        return true;
    default:
        return false;
    }
}

static void fn_reg_apply_mode_tags(FnSlave *fn)
{
    static const uint16_t formed[] = {
        0x03E9, 0x03EA, 0x0420, 0x04C5, 0x050A
    };
    unsigned i;
    uint8_t tlv[16];
    unsigned tlv_len = 0;

    for (i = 0; i < sizeof(formed) / sizeof(formed[0]); i++) {
        if (fn_reg_fill_mode_tag(fn, formed[i], tlv, &tlv_len, sizeof(tlv))) {
            unsigned n;

            if (tlv_len < 4) {
                continue;
            }
            n = (unsigned)tlv[2] | ((unsigned)tlv[3] << 8);
            fn_reg_upsert(fn, formed[i], tlv + 4, (uint16_t)n);
        } else {
            fn_reg_delete(fn, formed[i]);
        }
    }
    /* 1036 только при автоматическом режиме и непустом номере. */
    if ((fn->work_mode & 0x04) == 0) {
        fn_reg_delete(fn, 0x040C);
    } else {
        unsigned off = 0, full = 0;

        if (fn_tlv_find(fn->reg_tlv, fn->reg_tlv_len, 0x040C, &off, &full) &&
            (full < 4 || !fn_tlv_payload_present(fn->reg_tlv + off + 4,
                                                 full - 4))) {
            fn_reg_delete(fn, 0x040C);
        }
    }
}

static void fn_reg_capture_a3(FnSlave *fn)
{
    fn_reg_upsert(fn, 0x03FA, (const uint8_t *)fn->inn, 12);
    fn_reg_upsert(fn, 0x040D, (const uint8_t *)fn->rnm, 20);
    fn_reg_upsert(fn, 0x0411, (const uint8_t *)fn->sn, 16);
    fn_reg_upsert(fn, 0x041F, &fn->tax_system, 1);
    fn_reg_upsert(fn, 0x04B9, &fn->ffd_version, 1);
    if (fn_ascii_has_digit(fn->ofd_inn, 12)) {
        fn_reg_upsert(fn, 0x03F9, (const uint8_t *)fn->ofd_inn, 12);
    }
    if (fn->reason_code) {
        uint8_t rc[4];

        rc[0] = fn->reason_code & 0xFF;
        rc[1] = (fn->reason_code >> 8) & 0xFF;
        rc[2] = (fn->reason_code >> 16) & 0xFF;
        rc[3] = (fn->reason_code >> 24) & 0xFF;
        fn_reg_upsert(fn, 0x04B5, rc, 4); /* 1205 */
    }
    fn_reg_apply_mode_tags(fn);
}

static void fn_build_response(FnSlave *fn, uint8_t cmd,
                              const uint8_t *req, size_t req_len)
{
    switch (cmd) {
    case 0x30: {
        uint8_t st[30];
        fn_fill_status(fn, st);
        fn_emit_frame(fn, 0x00, st, sizeof(st));
        return;
    }
    case 0x31:
        fn_emit_frame(fn, 0x00, (const uint8_t *)fn->sn, 16);
        return;
    case 0x32: {
        uint8_t exp[3] = {30, 8, 12};
        fn_emit_frame(fn, 0x00, exp, sizeof(exp));
        return;
    }
    case 0x33: {
        /* Soft version 16 ASCII + type: 0 debug, 1 production (FF04 / FN 33h). */
        uint8_t ver[17];
        static const char vstr[] = "fn 1.2 mgm 03";

        memset(ver, ' ', 16);
        memcpy(ver, vstr, sizeof(vstr) - 1);
        ver[16] = 0x00;
        fn_emit_frame(fn, 0x00, ver, sizeof(ver));
        return;
    }
    case 0x34: {
        uint8_t life[8] = {0x5A, 0x00, 0x00, 0x00, 0x10, 0x27, 0x00, 0x00};
        fn_emit_frame(fn, 0x00, life, sizeof(life));
        return;
    }
    case 0x35: {
        uint8_t err[16] = {};
        fn_emit_frame(fn, 0x00, err, sizeof(err));
        return;
    }
    case 0x3A: {
        uint8_t fmt[2];

        fmt[0] = ((fn->life_phase & 0x02) == 0) ? 0 : fn->ffd_version;
        fmt[1] = 4;
        fn_emit_frame(fn, 0x00, fmt, sizeof(fmt));
        return;
    }
    case 0x3B: {
        uint8_t days[2] = {0x6D, 0x01}; /* 365 */

        if (req_len >= 5) {
            fn_store_dt(fn, req, req_len);
        }
        fn_emit_frame(fn, 0x00, days, sizeof(days));
        return;
    }
    case 0x3D: {
        uint8_t mem[9] = {};

        mem[0] = 0x10;
        mem[1] = 0x27; /* 10000 docs */
        mem[4] = 0x00;
        mem[5] = 0x04; /* 1024 KiB */
        mem[8] = 0;
        fn_emit_frame(fn, 0x00, mem, sizeof(mem));
        return;
    }
    case 0x3F: {
        uint8_t exe[48];

        memset(exe, ' ', sizeof(exe));
        memcpy(exe, "FN-1.2 MGM 03", 13);
        fn_emit_frame(fn, 0x00, exe, sizeof(exe));
        return;
    }
    case 0x36: {
        uint8_t ctr[354] = {};
        uint16_t sh = (uint16_t)fn->shift_number;

        memcpy(ctr, &sh, 2);
        fn_emit_frame(fn, 0x00, ctr, sizeof(ctr));
        return;
    }
    case 0x37: {
        uint8_t ctr[46] = {};
        uint16_t sh = (uint16_t)fn->shift_number;

        memcpy(ctr, &sh, 2);
        fn_emit_frame(fn, 0x00, ctr, sizeof(ctr));
        return;
    }
    case 0x38:
        fn_emit_zeros(fn, 86);
        return;
    case 0x39:
        fn_emit_zeros(fn, 44);
        return;
    case 0x06:
        fn_clear_doc(fn);
        fn->km_phase = 1;
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0x24:
        fn->ofd_reading = 0;
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0x25:
        if (!fn->ofd_reading) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        fn->ofd_reading = 0;
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0xA7: {
        uint8_t sz[8] = {};
        uint32_t n = fn->doc_tlv_len;

        memcpy(sz, &n, 4);
        fn_emit_frame(fn, 0x00, sz, sizeof(sz));
        return;
    }
    case 0xAB:
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0x21:
        if (!fn_need_fiscal(fn)) {
            return;
        }
        fn->ofd_transport = (req && req_len && req[0]) ? 1 : 0;
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0x20: {
        uint8_t ofd[13] = {};
        if (!fn_need_fiscal(fn)) {
            return;
        }
        if (fn->ofd_transport) {
            ofd[0] |= 1u;
        }
        if (fn->ofd_qn) {
            ofd[0] |= 2u;
            ofd[2] = fn->ofd_qn & 0xFF;
            ofd[3] = (fn->ofd_qn >> 8) & 0xFF;
            memcpy(ofd + 4, &fn->ofd_queue[0], 4);
            memcpy(ofd + 8, fn->last_dt, 5);
        }
        ofd[1] = fn->ofd_reading;
        fn_emit_frame(fn, 0x00, ofd, sizeof(ofd));
        return;
    }
    case 0x07:
        if (fn->cur_doc == 0) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (req && req_len) {
            if (fn->doc_tlv_len + req_len > FN_TLV_MAX) {
                fn_emit_frame(fn, 0x10, NULL, 0);
                return;
            }
            memcpy(fn->doc_tlv + fn->doc_tlv_len, req, req_len);
            fn->doc_tlv_len += req_len;
            fn->doc_data = 1;
            if (fn_is_reg_doc(fn->cur_doc)) {
                if (fn->reg_tlv_len + req_len > FN_TLV_MAX) {
                    fn_emit_frame(fn, 0x10, NULL, 0);
                    return;
                }
                memcpy(fn->reg_tlv + fn->reg_tlv_len, req, req_len);
                fn->reg_tlv_len += req_len;
            }
        }
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0x02:
    case 0xA2: {
        uint8_t report = (req && req_len >= 1) ? req[0] : 0;
        uint8_t kind;
        bool fiscal = (fn->life_phase & 0x02) != 0;

        if ((fn->life_phase & 0x01) == 0) {
            fn->life_phase = 0x01;
        }
        /* 0 регистрация, 1 замена ФН, 2 перерегистрация без замены ФН. */
        if (report > 2 || fn->cur_doc != 0 || (fn->life_phase & 0x04) != 0) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (report == 2) {
            if (!fiscal || fn->shift_open) {
                fn_emit_frame(fn, 0x02, NULL, 0);
                return;
            }
        } else if (fiscal) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (cmd == 0xA2 && req_len >= 2) {
            if (req[1] != 3 && req[1] != 4) {
                fn_emit_frame(fn, 0x0A, NULL, 0);
                return;
            }
            fn->ffd_version = req[1];
        } else if (cmd != 0xA2) {
            fn->ffd_version = 2;
        }
        kind = (report == 1) ? 0x12 : (report == 2) ? 0x13 : 0x01;
        fn->reg_tlv_len = 0;
        fn_begin_doc(fn, kind);
        fn_nv_save(fn);
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    }
    case 0x03:
    case 0xA3: {
        uint8_t dtype = fn->cur_doc;

        if (!fn_is_reg_doc(dtype)) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (req_len >= 5) {
            fn_store_dt(fn, req, req_len);
        }
        if (req_len >= 17) {
            memcpy(fn->inn, req + 5, 12);
            fn->inn[12] = 0;
        }
        if (req_len >= 37) {
            memcpy(fn->rnm, req + 17, 20);
            fn->rnm[20] = 0;
        }
        if (req_len >= 38) {
            fn->tax_system = req[37];
        }
        if (req_len >= 39) {
            fn->work_mode = req[38];
        }
        if (req_len >= 40) {
            fn->ext_flags = req[39];
        }
        if (req_len >= 52) {
            memcpy(fn->ofd_inn, req + 40, 12);
            fn->ofd_inn[12] = 0;
        }
        if (req_len >= 56) {
            memcpy(&fn->reason_code, req + 52, 4);
        } else if (dtype == 0x12) {
            fn->reason_code = 1;
        } else if (dtype == 0x01) {
            fn->reason_code = 0;
        }
        fn_reg_capture_a3(fn);
        fn->life_phase = 0x03;
        fn->fiscal_open = true;
        fn->last_fd_no++;
        fn_next_fp(fn);
        fn_archive_doc(fn, dtype, 0, 0);
        fn_clear_doc(fn);
        fn_emit_fd_fp(fn);
        return;
    }
    case 0x04:
        if (!fn_need_fiscal(fn)) {
            return;
        }
        fn_begin_doc(fn, 0x10);
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0x05:
        if (fn->cur_doc != 0x10) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (req_len >= 5) {
            fn_store_dt(fn, req, req_len);
        }
        if (req_len >= 25) {
            memcpy(fn->rnm, req + 5, 20);
            fn->rnm[20] = 0;
        }
        fn->life_phase = 0x07;
        fn->fiscal_open = false;
        fn->shift_open = false;
        fn->last_fd_no++;
        fn_next_fp(fn);
        fn_archive_doc(fn, 0x10, 0, 0);
        fn_clear_doc(fn);
        fn_emit_fd_fp(fn);
        return;
    case 0x10: {
        uint8_t out[5];
        if (!fn_need_fiscal(fn)) {
            return;
        }
        out[0] = fn->shift_open ? 1 : 0;
        out[1] = fn->shift_number & 0xFF;
        out[2] = (fn->shift_number >> 8) & 0xFF;
        out[3] = fn->receipt_in_shift & 0xFF;
        out[4] = (fn->receipt_in_shift >> 8) & 0xFF;
        fn_emit_frame(fn, 0x00, out, sizeof(out));
        return;
    }
    case 0x11:
        if (!fn_need_fiscal(fn) || fn->shift_open) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (req_len >= 5) {
            fn_store_dt(fn, req, req_len);
        }
        fn_begin_doc(fn, 0x02);
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0x12: {
        uint8_t out[10];
        if (fn->cur_doc != 0x02) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        fn->shift_open = true;
        fn->shift_number++;
        fn->receipt_in_shift = 0;
        fn->last_fd_no++;
        fn_next_fp(fn);
        fn_archive_doc(fn, 0x02, 0, 0);
        fn_clear_doc(fn);
        out[0] = fn->shift_number & 0xFF;
        out[1] = (fn->shift_number >> 8) & 0xFF;
        memcpy(out + 2, &fn->last_fd_no, 4);
        memcpy(out + 6, &fn->last_fp, 4);
        fn_emit_frame(fn, 0x00, out, sizeof(out));
        return;
    }
    case 0x13:
        if (!fn_need_fiscal(fn) || !fn->shift_open) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (req_len >= 5) {
            fn_store_dt(fn, req, req_len);
        }
        fn_begin_doc(fn, 0x08);
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0x14: {
        uint16_t closed;
        uint8_t out[10];
        if (fn->cur_doc != 0x08) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        closed = (uint16_t)fn->shift_number;
        fn->shift_open = false;
        fn->last_fd_no++;
        fn_next_fp(fn);
        fn_clear_doc(fn);
        out[0] = closed & 0xFF;
        out[1] = (closed >> 8) & 0xFF;
        memcpy(out + 2, &fn->last_fd_no, 4);
        memcpy(out + 6, &fn->last_fp, 4);
        fn_archive_doc(fn, 0x08, 0, 0);
        fn_clear_doc(fn);
        fn_emit_frame(fn, 0x00, out, sizeof(out));
        return;
    }
    case 0x15:
        if (!fn_need_fiscal(fn) || !fn->shift_open || fn->cur_doc != 0) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (req_len >= 5) {
            fn_store_dt(fn, req, req_len);
        }
        fn_begin_doc(fn, 0x04);
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0x16: {
        uint8_t out[10];
        uint8_t dtype = fn->cur_doc;

        if (dtype != 0x04 && dtype != 0x14) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (req_len >= 5) {
            fn_store_dt(fn, req, req_len);
        }
        fn->receipt_in_shift++;
        fn->last_fd_no++;
        fn_next_fp(fn);
        fn_archive_doc(fn, dtype, (req_len >= 6) ? req[5] : 1,
                       (req_len >= 12) ? (uint32_t)req[6] : 0);
        fn_clear_doc(fn);
        fn->km_phase = 1;
        fn->km_saved = 0;
        out[0] = fn->receipt_in_shift & 0xFF;
        out[1] = (fn->receipt_in_shift >> 8) & 0xFF;
        memcpy(out + 2, &fn->last_fd_no, 4);
        memcpy(out + 6, &fn->last_fp, 4);
        fn_emit_frame(fn, 0x00, out, sizeof(out));
        return;
    }
    case 0x17:
        if (!fn_need_fiscal(fn) || !fn->shift_open || fn->cur_doc != 0) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (req_len >= 5) {
            fn_store_dt(fn, req, req_len);
        }
        fn_begin_doc(fn, 0x14);
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0x18:
        if (!fn_need_fiscal(fn) || fn->cur_doc != 0) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (req_len >= 5) {
            fn_store_dt(fn, req, req_len);
        }
        fn_begin_doc(fn, 0x17);
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0x19: {
        uint8_t out[15] = {};

        if (fn->cur_doc != 0x17) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        fn->last_fd_no++;
        fn_next_fp(fn);
        fn_archive_doc(fn, 0x17, 0, 0);
        fn_clear_doc(fn);
        memcpy(out, &fn->last_fd_no, 4);
        memcpy(out + 4, &fn->last_fp, 4);
        out[8] = fn->ofd_qn & 0xFF;
        out[9] = (fn->ofd_qn >> 8) & 0xFF;
        memcpy(out + 12, fn->last_dt, 3);
        fn_emit_frame(fn, 0x00, out, sizeof(out));
        return;
    }
    case 0x22: {
        unsigned i;
        uint8_t lenb[2];

        if (!fn_need_fiscal(fn)) {
            return;
        }
        fn->ofd_msg_len = 0;
        fn->ofd_read_off = 0;
        if (!fn->ofd_qn) {
            fn_emit_frame(fn, 0x08, NULL, 0);
            return;
        }
        {
            uint32_t fd = fn->ofd_queue[0];
            FnArchivedDoc *d = fn_find_doc(fn, fd);

            fn->ofd_msg[fn->ofd_msg_len++] = 0xFF;
            for (i = 0; i < 4; i++) {
                fn->ofd_msg[fn->ofd_msg_len++] = (uint8_t)(fd >> (8 * i));
            }
            if (d) {
                fn->ofd_msg[fn->ofd_msg_len++] = d->type;
            }
            fn->ofd_reading = 1;
        }
        lenb[0] = fn->ofd_msg_len & 0xFF;
        lenb[1] = (fn->ofd_msg_len >> 8) & 0xFF;
        fn_emit_frame(fn, 0x00, lenb, 2);
        return;
    }
    case 0x23: {
        uint16_t off = 0, maxlen = 64;
        uint8_t chunk[66];
        unsigned n;

        if (req && req_len >= 2) {
            off = (uint16_t)(req[0] | (req[1] << 8));
        }
        if (req && req_len >= 4) {
            maxlen = (uint16_t)(req[2] | (req[3] << 8));
        }
        if (off >= fn->ofd_msg_len) {
            chunk[0] = 0;
            chunk[1] = 0;
            fn_emit_frame(fn, 0x00, chunk, 2);
            return;
        }
        n = fn->ofd_msg_len - off;
        if (n > maxlen) {
            n = maxlen;
        }
        if (n > 64) {
            n = 64;
        }
        chunk[0] = n & 0xFF;
        chunk[1] = (n >> 8) & 0xFF;
        memcpy(chunk + 2, fn->ofd_msg + off, n);
        fn_emit_frame(fn, 0x00, chunk, n + 2);
        return;
    }
    case 0x26:
        if (fn->ofd_qn) {
            FnArchivedDoc *d = fn_find_doc(fn, fn->ofd_queue[0]);

            if (d) {
                d->ofd_ack = 1;
            }
            memmove(fn->ofd_queue, fn->ofd_queue + 1,
                    sizeof(fn->ofd_queue[0]) * (fn->ofd_qn - 1));
            fn->ofd_qn--;
        }
        fn->ofd_reading = 0;
        fn->ofd_msg_len = 0;
        fn_nv_save(fn);
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0x40: {
        uint32_t fd = 0;
        FnArchivedDoc *d;
        uint8_t out[16];

        if (req && req_len >= 4) {
            memcpy(&fd, req, 4);
        }
        d = fn_find_doc(fn, fd);
        if (!d) {
            fn_emit_frame(fn, 0x08, NULL, 0);
            return;
        }
        memset(out, 0, sizeof(out));
        out[0] = d->type;
        out[1] = d->ofd_ack;
        memcpy(out + 2, &d->fd_no, 4);
        memcpy(out + 6, &d->fp, 4);
        fn_emit_frame(fn, 0x00, out, 10);
        return;
    }
    case 0x41: {
        uint8_t ticket[8] = {0x01, 0x00};
        fn_emit_frame(fn, 0x00, ticket, sizeof(ticket));
        return;
    }
    case 0x42: {
        uint8_t out[2];

        out[0] = fn->ofd_qn & 0xFF;
        out[1] = (fn->ofd_qn >> 8) & 0xFF;
        fn_emit_frame(fn, 0x00, out, 2);
        return;
    }
    case 0x43: {
        uint8_t full[64];
        unsigned n = 0;
        bool with_reason = req_len >= 1;

        if ((fn->life_phase & 0x02) == 0) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        memset(full, 0, sizeof(full));
        memcpy(full + n, fn->last_dt, 5);
        n += 5;
        memcpy(full + n, fn->inn, 12);
        n += 12;
        memcpy(full + n, fn->rnm, 20);
        n += 20;
        full[n++] = fn->tax_system;
        full[n++] = fn->work_mode;
        if (fn->ffd_version >= 3) {
            full[n++] = fn->ext_flags;
            memcpy(full + n, fn->ofd_inn, 12);
            n += 12;
            if (with_reason) {
                memcpy(full + n, &fn->reason_code, 4);
                n += 4;
            }
        }
        memcpy(full + n, &fn->last_fd_no, 4);
        n += 4;
        memcpy(full + n, &fn->last_fp, 4);
        n += 4;
        fn_emit_frame(fn, 0x00, full, n);
        return;
    }
    case 0x44: {
        uint16_t tag = 0;
        uint8_t tlv[256];
        unsigned tlv_len = 0;
        unsigned off = 0, full = 0;

        if ((fn->life_phase & 0x02) == 0) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (req && req_len >= 3) {
            tag = fn_u16le(req + 1);
        } else if (req && req_len >= 2) {
            tag = fn_u16le(req);
        }
        if (tag == 0xFFFF) {
            fn->tlv_read_kind = 2;
            fn->tlv_read_off = 0;
            fn_emit_frame(fn, 0x00, NULL, 0);
            return;
        }
        /* Mode flags are formed by the FN from A3, not from leftover 07h TLVs. */
        if (fn_reg_fill_mode_tag(fn, tag, tlv, &tlv_len, sizeof(tlv))) {
            fn_emit_frame(fn, 0x00, tlv, tlv_len);
            return;
        }
        if (tag == 0x03E9 || tag == 0x03EA || tag == 0x0420 ||
            tag == 0x04C5 || tag == 0x050A) {
            fn_emit_frame(fn, 0x08, NULL, 0);
            return;
        }
        if (tag == 0x040C) {
            if ((fn->work_mode & 0x04) &&
                fn_tlv_find(fn->reg_tlv, fn->reg_tlv_len, tag, &off, &full) &&
                full > 4 &&
                fn_tlv_payload_present(fn->reg_tlv + off + 4, full - 4)) {
                fn_emit_frame(fn, 0x00, fn->reg_tlv + off, full);
                return;
            }
            fn_emit_frame(fn, 0x08, NULL, 0);
            return;
        }
        if (fn_tlv_find(fn->reg_tlv, fn->reg_tlv_len, tag, &off, &full)) {
            fn_emit_frame(fn, 0x00, fn->reg_tlv + off, full);
            return;
        }
        if (tag == 0x03FA && fn_ascii_has_digit(fn->inn, 12)) {
            fn_push_tlv(tlv, &tlv_len, sizeof(tlv), tag,
                        (const uint8_t *)fn->inn, 12);
        } else if (tag == 0x040D && fn_ascii_has_digit(fn->rnm, 20)) {
            fn_push_tlv(tlv, &tlv_len, sizeof(tlv), tag,
                        (const uint8_t *)fn->rnm, 20);
        } else if (tag == 0x0411) {
            fn_push_tlv(tlv, &tlv_len, sizeof(tlv), tag,
                        (const uint8_t *)fn->sn, 16);
        } else if (tag == 0x03F9 && fn_ascii_has_digit(fn->ofd_inn, 12)) {
            fn_push_tlv(tlv, &tlv_len, sizeof(tlv), tag,
                        (const uint8_t *)fn->ofd_inn, 12);
        } else if (tag == 0x041F) {
            fn_push_tlv(tlv, &tlv_len, sizeof(tlv), tag, &fn->tax_system, 1);
        } else if (tag == 0x04B9 || tag == 0x04A5) {
            fn_push_tlv(tlv, &tlv_len, sizeof(tlv), tag, &fn->ffd_version, 1);
        } else if (tag == 0x0424) {
            static const uint8_t fns[] = "www.nalog.gov.ru";
            fn_push_tlv(tlv, &tlv_len, sizeof(tlv), tag, fns, 16);
        } else {
            fn_emit_frame(fn, 0x08, NULL, 0);
            return;
        }
        fn_emit_frame(fn, 0x00, tlv, tlv_len);
        return;
    }
    case 0x45: {
        uint32_t fd = 0;
        FnArchivedDoc *d;
        uint8_t out[4] = {};

        if ((fn->life_phase & 0x02) == 0) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (req && req_len >= 4) {
            memcpy(&fd, req, 4);
        }
        d = fn_find_doc(fn, fd);
        if (!d) {
            fn_emit_frame(fn, 0x08, NULL, 0);
            return;
        }
        out[0] = d->type;
        fn->tlv_read_kind = 1;
        fn_emit_frame(fn, 0x00, out, sizeof(out));
        return;
    }
    case 0x46:
        if (fn->tlv_read_kind != 1) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        fn->tlv_read_kind = 0;
        fn_emit_frame(fn, 0x08, NULL, 0);
        return;
    case 0x47: {
        unsigned full = 0;

        if (fn->tlv_read_kind != 2) {
            fn_emit_frame(fn, 0x08, NULL, 0);
            return;
        }
        if (!fn_tlv_at(fn->reg_tlv, fn->reg_tlv_len, fn->tlv_read_off, &full)) {
            fn->tlv_read_kind = 0;
            fn->tlv_read_off = 0;
            fn_emit_frame(fn, 0x08, NULL, 0);
            return;
        }
        fn_emit_frame(fn, 0x00, fn->reg_tlv + fn->tlv_read_off, full);
        fn->tlv_read_off += full;
        return;
    }
    case 0x50: {
        uint32_t fd = 0;
        FnArchivedDoc *d;
        uint8_t out[11] = {};

        if (req && req_len >= 4) {
            memcpy(&fd, req, 4);
        }
        d = fn_find_doc(fn, fd);
        if (!d) {
            fn_emit_frame(fn, 0x08, NULL, 0);
            return;
        }
        out[0] = 0;
        out[1] = d->type;
        out[2] = d->ofd_ack;
        memcpy(out + 3, &d->fd_no, 4);
        memcpy(out + 7, &d->fp, 4);
        fn_emit_frame(fn, 0x00, out, sizeof(out));
        return;
    }
    case 0x60: {
        uint8_t code = (req && req_len) ? req[0] : 0;
        fn_clear_doc(fn);
        fn->ofd_reading = 0;
        if (code == 22) {
            fn_factory_clean(fn);
        } else {
            fn->shift_open = false;
        }
        fn_nv_save(fn);
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    }
    case 0xB0: {
        uint8_t st[9] = {};

        if (!fn_need_marking(fn)) {
            return;
        }
        if (req && req_len && req[0] == 0) {
            uint8_t allow = 0;

            fn_emit_frame(fn, 0x00, &allow, 1);
            return;
        }
        st[0] = fn->km_phase;
        st[1] = 0;
        st[2] = 0xFF;
        st[3] = fn->km_saved;
        st[4] = 0;
        st[5] = 0;
        fn_emit_frame(fn, 0x00, st, sizeof(st));
        return;
    }
    case 0xB1: {
        uint8_t r[2] = {0, 1};

        if (!fn_need_marking(fn)) {
            return;
        }
        if (!fn->shift_open) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        fn->km_phase = 2;
        fn_emit_frame(fn, 0x00, r, sizeof(r));
        return;
    }
    case 0xB2: {
        uint8_t save = (req && req_len) ? req[0] : 0;
        uint8_t res = 0;

        if (!fn_need_marking(fn)) {
            return;
        }
        if (fn->km_phase < 2) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (save == 1) {
            if (fn->km_saved >= 128) {
                fn_emit_frame(fn, 0x35, NULL, 0);
                return;
            }
            fn->km_saved++;
            fn->km_phase = 1;
            fn_emit_frame(fn, 0x00, &res, 1);
            return;
        }
        fn->km_phase = 1;
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    }
    case 0xB3:
        if (!fn_need_marking(fn)) {
            return;
        }
        fn->km_saved = 0;
        fn->km_phase = 1;
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0xB5:
        if (!fn_need_marking(fn)) {
            return;
        }
        if (req_len == 0) {
            if (fn->km_phase < 3) {
                fn_emit_frame(fn, 0x33, NULL, 0);
                return;
            }
            fn_emit_protected_stub(fn, 8);
            return;
        }
        if (fn->km_phase < 2) {
            fn_emit_frame(fn, 0x33, NULL, 0);
            return;
        }
        if (req_len >= 5) {
            fn_store_dt(fn, req, req_len);
        }
        fn->km_phase = 3;
        fn_emit_protected_stub(fn, 8);
        return;
    case 0xB6: {
        uint8_t res = 0;

        if (!fn_need_marking(fn)) {
            return;
        }
        if (fn->km_phase != 3) {
            fn_emit_frame(fn, 0x33, NULL, 0);
            return;
        }
        fn->km_phase = 4;
        fn_emit_frame(fn, 0x00, &res, 1);
        return;
    }
    case 0xB7: {
        uint8_t extra = (req && req_len) ? req[0] : 0;

        if (!fn_need_marking(fn)) {
            return;
        }
        if (fn->cur_doc != 0x04 && fn->cur_doc != 0x14) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        if (extra < 1 || extra > 3 || req_len < 2) {
            fn_emit_frame(fn, 0x01, NULL, 0);
            return;
        }
        if (fn->doc_tlv_len + (req_len - 1) > FN_TLV_MAX) {
            fn_emit_frame(fn, 0x10, NULL, 0);
            return;
        }
        memcpy(fn->doc_tlv + fn->doc_tlv_len, req + 1, req_len - 1);
        fn->doc_tlv_len += (unsigned)(req_len - 1);
        fn->doc_data = 1;
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    }
    case 0xBA: {
        uint8_t st[13] = {};

        if (!fn_need_marking(fn)) {
            return;
        }
        st[0] = fn->notif_xfer;
        fn_emit_frame(fn, 0x00, st, sizeof(st));
        return;
    }
    case 0xBB:
        if (!fn_need_marking(fn)) {
            return;
        }
        if (fn->notif_xfer == 1) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        fn_emit_frame(fn, 0x08, NULL, 0);
        return;
    case 0xBC:
        if (!fn_need_marking(fn)) {
            return;
        }
        if (fn->notif_xfer != 1) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        fn_emit_frame(fn, 0x08, NULL, 0);
        return;
    case 0xBD:
        if (!fn_need_marking(fn)) {
            return;
        }
        fn->notif_xfer = 0;
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0xBE:
        if (!fn_need_marking(fn)) {
            return;
        }
        if (fn->notif_xfer != 1) {
            fn_emit_frame(fn, 0x33, NULL, 0);
            return;
        }
        fn->notif_xfer = 2;
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0xBF:
        if (!fn_need_marking(fn)) {
            return;
        }
        if (fn->notif_xfer != 2) {
            fn_emit_frame(fn, 0x02, NULL, 0);
            return;
        }
        fn->notif_xfer = 0;
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0xD0:
        if (!fn_need_fiscal(fn) || !fn_need_marking(fn)) {
            return;
        }
        if (req_len >= 5) {
            fn_store_dt(fn, req, req_len);
        }
        fn_emit_protected_stub(fn, 8);
        return;
    case 0xD1:
        if (!fn_need_marking(fn)) {
            return;
        }
        fn_emit_frame(fn, 0x00, NULL, 0);
        return;
    case 0xD3:
    case 0xD4:
    case 0xD5:
    case 0xD6:
        /* Not autonomous: OFD transport mode. */
        fn_emit_frame(fn, 0x02, NULL, 0);
        return;
    case 0xD7: {
        uint8_t code = (req && req_len) ? req[0] : 0;
        static const char uri[] = "okp.fn.local";

        if (!fn_need_fiscal(fn) || !fn_need_marking(fn)) {
            return;
        }
        if (code == 0) {
            uint8_t flag = 0;

            fn_emit_frame(fn, 0x00, &flag, 1);
            return;
        }
        if (code != 1) {
            fn_emit_frame(fn, 0x01, NULL, 0);
            return;
        }
        fn_emit_frame(fn, 0x00, (const uint8_t *)uri, sizeof(uri) - 1);
        return;
    }
    default:
        fn_emit_frame(fn, 0x01, NULL, 0);
        return;
    }
}

void fn_slave_on_master_write(FnSlave *fn, const uint8_t *data, size_t len)
{
    uint16_t body;
    uint8_t cmd, answer;
    const uint8_t *payload;
    size_t payload_len;

    if (!data || len == 0) {
        return;
    }
    if (len < 4 || data[0] != 0x04) {
        fn_log_line(fn, true, 0, data, len, 0, NULL, 0, true,
                    "нет STX 04h или кадр короче 4 байт");
        return;
    }
    body = (uint16_t)(data[1] | (data[2] << 8));
    if (body < 1 || len < (size_t)(3 + body + 2)) {
        fn_log_line(fn, true, 0, data, len, 0, NULL, 0, true,
                    "длина тела не совпадает с кадром");
        return;
    }
    /* Host CRC often ≠ spec; still execute. Reply CRC is always correct. */
    cmd = data[3];
    payload = (body > 1) ? (data + 4) : NULL;
    payload_len = (body > 1) ? (size_t)(body - 1) : 0;
    fn->last_cmd = cmd;
    fn_log_line(fn, true, cmd, data, 3 + body + 2, 0, payload, payload_len,
                false, NULL);
    fn_build_response(fn, cmd, payload, payload_len);
    if (fn->rx_len >= 4) {
        body = (uint16_t)(fn->rx[1] | (fn->rx[2] << 8));
        answer = fn->rx[3];
        payload = (body > 1) ? (fn->rx + 4) : NULL;
        payload_len = (body > 1) ? (size_t)(body - 1) : 0;
        fn_log_line(fn, false, fn->last_cmd, fn->rx, fn->rx_len,
                    answer, payload, payload_len, false, NULL);
    }
}
