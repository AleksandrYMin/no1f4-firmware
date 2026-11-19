/* No.1 F4 — минимальное собственное приложение.
 *
 * nRF51422 + S110 v8.0.0, приложение с 0x18000, заводской загрузчик Nordic
 * (legacy DFU). Обновление по воздуху — как в заводской прошивке:
 * команда 0x42 в FEA1 -> GPREGRET = 0xB1, SoftDevice выключен, векторы
 * на загрузчик -> прямой переход в загрузчик (не сброс!).
 *
 * Сервис C3E6FEA0-E966-1000-8000-BE99C223DF6A:
 *   FEA1 (write, write without response) — команды;
 *   FEA2 (read, notify)                  — ответ на последнюю команду.
 *
 * Команды (первый байт FEA1):
 *   0x01                    ping -> 01 "версия"
 *   0x10 addr[4 LE] n       чтение памяти, n <= 15 -> 10 addr[4] data[n]
 *   0x20 on                 пульсометр вкл/выкл (P0.07) -> 20 on
 *   0x30 pin level          выход из SENSORS_OUT_MASK -> 30 pin level
 *   0x31 ms[2 LE]           вибрация ms (0 — выкл), плавный пуск -> 31 ms[2]
 *   0x40                    JEDEC ID внешней флеш -> 40 id[3]
 *   0x41 addr[3] len[3] LE  чтение внешней флеш потоком -> 41 addr[3] len[3], затем 44 data...
 *                           (len = 0 — остановить поток)
 *   0x42                    перезагрузка в DFU (ответа нет, связь рвётся)
 * Зонд неизвестного дисплея (0.5+; cs/dc — номера пинов, dc = FF — нет D/C;
 * flags: биты 0–1 режим SPI, бит 2 — только данные, бит 3 — чтение по SDA):
 *   0x32 pin mode           режим пина (0 вход, 1 вверх, 2 вниз, 3 выход 0, 4 выход 1)
 *   0x33 pin sense          поле SENSE пина (0 выкл, 2 по 1, 3 по 0), сброс EVENTS_PORT -> 33 pin sense
 *   0x34                    было ли GPIOTE PORT (сигнал DETECT, тот же, что будит из System OFF)
 *                           -> 34 event GPIO.IN[4 LE], событие сбрасывается
 *                           -> 32 pin mode GPIO.IN[4 LE]
 *   0x50 cs dc flags b...   команда b[0] (D/C = 0) и параметры (D/C = 1), до 17 байт -> 50 n
 *   0x51 cs dc flags c[2 LE] cnt[3 LE]   RAMWR и cnt пикселей RGB565 цвета c -> 51
 *   0x52 cs dc flags cmd n  команда и n <= 16 байт ответа -> 52 cmd data[n]
 * Запись во внешнюю флеш (0.8+), только в EXTFLASH_USER_START..END:
 *   0x45 addr[3 LE]         стереть сектор 4 КБ -> 45 addr[3]
 *   0x46 addr[3 LE] d[..16] записать, не пересекая страницу 256 байт -> 46 n
 *   0x47                    регистры статуса флеш -> 47 sr1 sr2
 *   0x48                    временно снять защиту записи (50h + 01h) -> 48 sr1 sr2
 * Картинка (0.8+): долгое нажатие на странице STATUS выводит на 5 с IMAGE_ADDR, если там заголовок
 * "IMG1" w[2 LE] h[2 LE], дальше w*h пикселей RGB565 big-endian построчно.
 * Анимация (0.9+): "ANI1" w[2] h[2] n[2] ms[2] (LE), дальше n кадров подряд в
 * том же формате; крутится по кругу, пока показывается картинка.
 * Ошибка: EE <код команды>.
 * Сами по себе (FEA2, notify):
 *   21 in[4 LE]             изменились входы (кнопки, P0.28/29)
 *   22 seq s[9] (u16 LE)    отсчёты PPG (AIN2) с частотой TICK_HZ, пока пульс включён
 *   44 data[1..19]          данные внешней флеш по порядку, пока не отдан весь len
 *
 * Страховка: HardFault, сбой SoftDevice и ошибка инициализации уводят
 * браслет в DFU, а не в цикл перезагрузок без BLE. Из HardFault переход
 * сделать нельзя, поэтому там метка APP_DFU_REQUEST в GPREGRET и сброс,
 * а переход делает main() при старте.
 *
 * Меню (0.11+), сенсорная кнопка P0.11:
 *   касание погашенного экрана — включить его на странице STATUS;
 *   короткое (отпустили раньше LONG_PRESS_TICKS) — следующая страница;
 *   долгое — действие страницы, сразу по достижении порога, с длинной вибрацией.
 * Страницы: STATUS (входы, батарея, пульсометр, BLE; долгое — картинка),
 * CAMERA (долгое — Volume Up, спуск камеры), SCRIPTS и MOUSE (заготовки),
 * POWER (долгое — сон, см. ниже).
 *
 * Сон (0.12+): долгое нажатие на POWER, отпустить, через 0,5 с часы засыпают
 * (касание в эти 0,5 с отменяет; на зарядке сон не включается — она же будит).
 * Экран и флеш в своих режимах сна, SoftDevice выключен, System OFF (0.13+).
 * Выход — всегда сброс чипа и обычная загрузка с BLE: касание P0.11 или начало
 * зарядки P0.29 (DETECT). RESETREAS.OFF и метка SLEEP_MAGIC в GPREGRET дают
 * причину пробуждения на STATUS.
 * 0.12.x спали в System ON со страховочным WDT. WDT переживает программный
 * сброс, поэтому, если он ещё идёт, приложение его кормит, а сон идёт в System ON.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "nrf.h"
#include "nrf_sdm.h"
#include "nrf_soc.h"
#include "ble.h"
#include "ble_hci.h"

#include "bootloader_util.h"
#include "nrf_delay.h"
#include "display.h"
#include "extflash.h"
#include "sensors.h"
#include "hid.h"

#define DEVICE_NAME          "F4 Custom"
#define FW_VERSION           "0.13.0"

/* Для загрузчика SDK 10 значит «приложение уже выключило SoftDevice,
 * переключило векторы и перешло в меня». После настоящего сброса с этим
 * значением загрузчик не поднимает SoftDevice и возвращается в приложение. */
#define BOOTLOADER_DFU_START 0xB1
/* Своя метка «уйти в DFU при старте»: загрузчик её не трогает. */
#define APP_DFU_REQUEST      0x5A
/* Метка «часы спали». Загрузчик смотрит только на 0xB1 (RE_NOTES §6). */
#define SLEEP_MAGIC          0x5B
/* Заводская прошивка F4 использует кварц (8 = XTAL_20_PPM, 0x240F8), но на
 * плате цветной версии кварц не подтверждён: без него SoftDevice зависнет при
 * старте, и часы останутся без BLE и без пути в DFU. RC (9) работает на любой
 * плате; тот же источник выбирает заводской загрузчик. */
#define LFCLK_SRC            NRF_CLOCK_LFCLKSRC_RC_250_PPM_250MS_CALIBRATION

#define UUID_SERVICE         0xFEA0
#define UUID_CTRL            0xFEA1
#define UUID_RESP            0xFEA2

#define CMD_PING             0x01
#define CMD_PEEK             0x10
#define CMD_HR               0x20
#define CMD_GPIO             0x30
#define CMD_MOTOR            0x31
#define CMD_PIN              0x32
#define CMD_SENSE            0x33
#define CMD_SENSE_POLL       0x34
#define CMD_LCD_WRITE        0x50
#define CMD_LCD_FILL         0x51
#define CMD_LCD_READ         0x52
#define CMD_FLASH_ERASE      0x45
#define CMD_FLASH_WRITE      0x46
#define CMD_FLASH_STATUS     0x47
#define CMD_FLASH_UNPROTECT  0x48
#define CMD_FLASH_ID         0x40
#define CMD_FLASH_READ       0x41
#define CMD_DFU              0x42
#define EVT_FLASH            0x44
#define EVT_INPUTS           0x21
#define EVT_PPG              0x22
#define RESP_ERROR           0xEE

#define RESP_MAX             20  /* ATT MTU 23 - 3 */
#define PEEK_MAX             (RESP_MAX - 5)

#define ADV_INTERVAL         320 /* 200 мс, единицы 0.625 мс */

/* RTC1: 32768 / (PRESCALER + 1) ≈ 25 Гц. */
#define TICK_HZ              25
#define RTC_PRESCALER        (32768 / TICK_HZ - 1)
#define SCREEN_EVERY         6   /* перерисовка ~4 раза в секунду */
#define SCREEN_ON_TICKS      (10 * TICK_HZ) /* экран гаснет через 10 с без касаний */
#define PIN_TOUCH            11  /* сенсорная кнопка: 1 в покое, 0 при касании */
#define IMAGE_ADDR           EXTFLASH_USER_START
#define IMAGE_TICKS          (5 * TICK_HZ)
/* 3: в этом тике счётчик станет 2, мотор выключится через 1–2 тика (40–80 мс полного хода). */
#define TOUCH_BUZZ_TICKS     3
#define LONG_PRESS_TICKS     (TICK_HZ * 6 / 10) /* 0,6 с — долгое нажатие */
#define LONG_BUZZ_TICKS      8   /* ~300 мс: долгое нажатие сработало */
#define NOTE_TICKS           (2 * TICK_HZ)      /* сколько висит результат действия */
#define SLEEP_DELAY_TICKS    (TICK_HZ / 2)      /* от отпускания кнопки до сна */
#define WAKE_BUZZ_TICKS      5
#define PPG_PER_PACKET       ((RESP_MAX - 2) / 2)
#define TRACE_LEN            (DISPLAY_W - 2)

/* C3E6xxxx-E966-1000-8000-BE99C223DF6A, little-endian, байты 12–13 под 16-битный UUID. */
static const ble_uuid128_t m_base_uuid = {{
    0x6A, 0xDF, 0x23, 0xC2, 0x99, 0xBE, 0x00, 0x80,
    0x00, 0x10, 0x66, 0xE9, 0x00, 0x00, 0xE6, 0xC3
}};

static uint16_t                 m_conn = BLE_CONN_HANDLE_INVALID;
static ble_gatts_char_handles_t m_ctrl;
static ble_gatts_char_handles_t m_resp;
static bool                     m_dfu_pending;
static uint32_t                 m_hid_err = 1;  /* результат hid_init(); 0 — HID работает */
static uint32_t                 m_evt_buf[(sizeof(ble_evt_t) + GATT_MTU_SIZE_DEFAULT + 3) / 4];

static const char *             m_status = "ADV";
static uint32_t                 m_inputs;
static uint16_t                 m_ppg[PPG_PER_PACKET];
static uint8_t                  m_ppg_count;
static uint8_t                  m_ppg_seq;
static uint16_t                 m_trace[TRACE_LEN];
static uint8_t                  m_trace_pos;
static uint8_t                  m_tick;
static bool                     m_screen_on = true;
static uint16_t                 m_screen_left = SCREEN_ON_TICKS;
static uint16_t                 m_image_ticks;  /* сколько ещё показывать картинку */
/* Анимация: кадры с m_anim_base, по m_anim_size байт, смена раз в m_anim_period тиков. */
static uint32_t                 m_anim_base;
static uint32_t                 m_anim_size;
static uint16_t                 m_anim_w, m_anim_h, m_anim_count, m_anim_frame;
static uint8_t                  m_anim_period, m_anim_tick;
static uint16_t                 m_motor_ticks;  /* сколько тиков RTC1 ещё вибрировать */

enum { PAGE_STATUS, PAGE_CAMERA, PAGE_SCRIPTS, PAGE_MOUSE, PAGE_POWER, PAGE_COUNT };
static const char * const       m_page_title[PAGE_COUNT] = {
    "STATUS", "CAMERA", "SCRIPTS", "MOUSE", "POWER"
};
static uint8_t                  m_page;         /* PAGE_STATUS после каждого гашения экрана */
static uint8_t                  m_press_ticks;  /* сколько тиков держат кнопку, 0 — отпущена */
static bool                     m_press_used;   /* нажатие отработано: разбудило экран или было долгим */
static const char *             m_note;         /* результат последнего действия */
static uint8_t                  m_note_ticks;
static bool                     m_sleep_armed;  /* долгое на POWER: уснуть, когда отпустят */
static uint8_t                  m_sleep_ticks;  /* отсчёт до сна, 0 — не засыпаем */
static uint32_t                 m_resetreas;    /* RESETREAS при старте (затем очищен) */
static const char *             m_wake = "BOOT";

/* Поток чтения внешней флеш: пакет, который не влез в буферы SoftDevice, ждёт TX_COMPLETE. */
static uint32_t                 m_stream_addr;
static uint32_t                 m_stream_left;
static uint8_t                  m_stream_pkt[RESP_MAX];
static uint16_t                 m_stream_pkt_len;


/* Без SoftDevice API: годится для HardFault и assert, когда SVC вызывать нельзя. */
static void fault_to_dfu(void)
{
    NRF_POWER->GPREGRET = APP_DFU_REQUEST;
    NVIC_SystemReset();
}

/* Повторяет заводской 0x242D4 (bootloader_start() из SDK 10).
 * Можно звать и из прерывания: bootloader_util выходит из него сам. */
static void enter_dfu(void)
{
    uint8_t sd_enabled = 0;

    sd_softdevice_is_enabled(&sd_enabled);
    if (sd_enabled)
    {
        sd_power_gpregret_clr(0xFF);
        sd_power_gpregret_set(BOOTLOADER_DFU_START);
        sd_softdevice_disable();
    }
    else
    {
        NRF_POWER->GPREGRET = BOOTLOADER_DFU_START;
    }
    /* Загрузчик получает периферию как есть, без сброса. Его app_timer
     * пишет PRESCALER RTC1, а на работающем RTC1 запись игнорируется:
     * с нашим RTC1 загрузчик зависает (проверено на 0.3.0). */
    NRF_RTC1->TASKS_STOP = 1;
    nrf_delay_us(100); /* остановка — за 1 такт LFCLK */
    NRF_RTC1->TASKS_CLEAR = 1;
    NRF_RTC1->INTENCLR = 0xFFFFFFFF;
    NRF_RTC1->EVTENCLR = 0xFFFFFFFF;
    NRF_RTC1->EVENTS_TICK = 0;
    NRF_RTC1->PRESCALER = 0;
    sensors_deinit();

    sd_softdevice_vector_table_base_set(NRF_UICR->BOOTLOADERADDR);

    NVIC->ICER[0] = 0xFFFFFFFF;
    NVIC->ICPR[0] = 0xFFFFFFFF;
    bootloader_util_app_start(NRF_UICR->BOOTLOADERADDR);
}

#define CHECK(call) do { if ((call) != NRF_SUCCESS) enter_dfu(); } while (0)

void HardFault_Handler(void)
{
    fault_to_dfu();
}

static void sd_assert(uint32_t pc, uint16_t line, const uint8_t * p_file)
{
    (void)pc; (void)line; (void)p_file;
    fault_to_dfu();
}


static bool peek_allowed(uint32_t addr, uint32_t len)
{
    static const struct { uint32_t start, end; } ranges[] = {
        { 0x00018000, 0x00040000 }, /* приложение, загрузчик, его настройки */
        { 0x10000000, 0x10000400 }, /* FICR */
        { 0x10001000, 0x10001100 }, /* UICR */
        { 0x20002000, 0x20008000 }, /* RAM приложения */
        { 0x40000000, 0x40001000 }, /* CLOCK/POWER: RESETREAS, GPREGRET */
        { 0x40010400, 0x40010620 }, /* WDT: RUNSTATUS, CRV, RREN */
        { 0x50000500, 0x50000800 }, /* GPIO */
        { 0x40006000, 0x40006600 }, /* GPIOTE: события и конфигурация */
    };
    /* Флеш SoftDevice (< 0x18000) не читается из региона 1 — это HardFault. */
    for (uint32_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++)
    {
        if (addr >= ranges[i].start && addr + len <= ranges[i].end)
        {
            return true;
        }
    }
    return false;
}

/* Читаем словами: часть периферии не любит побайтовый доступ. */
static void peek(uint32_t addr, uint8_t * p_out, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        uint32_t a = addr + i;
        uint32_t word = *(volatile const uint32_t *)(a & ~3u);
        p_out[i] = (uint8_t)(word >> (8 * (a & 3)));
    }
}

static void respond(const uint8_t * p_data, uint16_t len)
{
    ble_gatts_value_t value = { .len = len, .offset = 0, .p_value = (uint8_t *)p_data };

    sd_ble_gatts_value_set(m_conn, m_resp.value_handle, &value);
    if (m_conn != BLE_CONN_HANDLE_INVALID)
    {
        ble_gatts_hvx_params_t hvx = {
            .handle = m_resp.value_handle,
            .type   = BLE_GATT_HVX_NOTIFICATION,
            .p_len  = &len,
            .p_data = (uint8_t *)p_data,
        };
        /* Уведомления могут быть не включены — тогда ответ только читается. */
        sd_ble_gatts_hvx(m_conn, &hvx);
    }
}

/* Шлём пакеты, пока SoftDevice принимает. Дальше — по BLE_EVT_TX_COMPLETE. */
static void stream_pump(void)
{
    while (m_conn != BLE_CONN_HANDLE_INVALID && (m_stream_left || m_stream_pkt_len))
    {
        uint32_t err;

        if (m_stream_pkt_len == 0)
        {
            uint32_t n = m_stream_left < RESP_MAX - 1 ? m_stream_left : RESP_MAX - 1;

            m_stream_pkt[0] = EVT_FLASH;
            extflash_read(m_stream_addr, &m_stream_pkt[1], n);
            m_stream_addr += n;
            m_stream_left -= n;
            m_stream_pkt_len = (uint16_t)(1 + n);
        }

        ble_gatts_hvx_params_t hvx = {
            .handle = m_resp.value_handle,
            .type   = BLE_GATT_HVX_NOTIFICATION,
            .p_len  = &m_stream_pkt_len,
            .p_data = m_stream_pkt,
        };
        err = sd_ble_gatts_hvx(m_conn, &hvx);
        if (err == BLE_ERROR_NO_TX_BUFFERS)
        {
            return;
        }
        if (err != NRF_SUCCESS)
        {
            /* Уведомления выключены или связь рвётся — поток бросаем. */
            m_stream_left = 0;
        }
        m_stream_pkt_len = 0;
    }
}

static void stream_stop(void)
{
    m_stream_left = 0;
    m_stream_pkt_len = 0;
}

static void motor_stop(void)
{
    m_motor_ticks = 0;
    sensors_motor(false);
}

static void on_command(const uint8_t * p_data, uint16_t len)
{
    uint8_t  r[RESP_MAX];
    uint16_t n;

    if (len == 0)
    {
        return;
    }

    switch (p_data[0])
    {
        case CMD_DFU:
            /* Сначала рвём связь, чтобы телефон получил ответ на запись. */
            m_dfu_pending = true;
            if (sd_ble_gap_disconnect(m_conn, BLE_HCI_REMOTE_USER_TERMINATED_CONNECTION) != NRF_SUCCESS)
            {
                enter_dfu();
            }
            return;

        case CMD_PING:
            r[0] = CMD_PING;
            n = 1 + sizeof(FW_VERSION) - 1;
            memcpy(&r[1], FW_VERSION, n - 1);
            break;

        case CMD_HR:
            if (len < 2)
            {
                goto error;
            }
            sensors_hr_enable(p_data[1] != 0);
            m_ppg_count = 0;
            r[0] = CMD_HR;
            r[1] = p_data[1] != 0;
            n = 2;
            break;

        case CMD_GPIO:
            if (len < 3 || !sensors_output(p_data[1], p_data[2] != 0))
            {
                goto error;
            }
            r[0] = CMD_GPIO;
            r[1] = p_data[1];
            r[2] = p_data[2] != 0;
            n = 3;
            break;

        case CMD_MOTOR:
        {
            uint32_t ms;

            if (len < 3)
            {
                goto error;
            }
            ms = p_data[1] | (p_data[2] << 8);
            if (ms == 0)
            {
                motor_stop();
            }
            else
            {
                if (m_motor_ticks == 0)
                {
                    sensors_motor(true);
                }
                m_motor_ticks = (uint16_t)((ms * TICK_HZ + 999) / 1000);
            }
            r[0] = CMD_MOTOR;
            r[1] = p_data[1];
            r[2] = p_data[2];
            n = 3;
            break;
        }

        case CMD_FLASH_ERASE:
        {
            uint32_t addr;

            if (len < 4)
            {
                goto error;
            }
            addr = p_data[1] | (p_data[2] << 8) | ((uint32_t)p_data[3] << 16);
            stream_stop();
            if (!extflash_erase_sector(addr))
            {
                goto error;
            }
            memcpy(r, p_data, 4);
            n = 4;
            break;
        }

        case CMD_FLASH_WRITE:
        {
            uint32_t addr;

            if (len < 5)
            {
                goto error;
            }
            addr = p_data[1] | (p_data[2] << 8) | ((uint32_t)p_data[3] << 16);
            stream_stop();
            if (!extflash_program(addr, &p_data[4], len - 4u))
            {
                goto error;
            }
            r[0] = CMD_FLASH_WRITE;
            r[1] = (uint8_t)(len - 4);
            n = 2;
            break;
        }

        case CMD_FLASH_STATUS:
            r[0] = CMD_FLASH_STATUS;
            extflash_status(&r[1]);
            n = 3;
            break;

        case CMD_FLASH_UNPROTECT:
            r[0] = CMD_FLASH_UNPROTECT;
            extflash_unprotect_volatile(&r[1]);
            n = 3;
            break;

        case CMD_FLASH_ID:
            r[0] = CMD_FLASH_ID;
            extflash_id(&r[1]);
            n = 4;
            break;

        case CMD_FLASH_READ:
            if (len < 7)
            {
                goto error;
            }
            stream_stop();
            m_stream_addr = p_data[1] | (p_data[2] << 8) | ((uint32_t)p_data[3] << 16);
            m_stream_left = p_data[4] | (p_data[5] << 8) | ((uint32_t)p_data[6] << 16);
            memcpy(r, p_data, 7);
            respond(r, 7);
            stream_pump();
            return;

        case CMD_PEEK:
        {
            uint32_t addr;
            uint8_t  cnt;

            if (len < 6)
            {
                goto error;
            }
            addr = p_data[1] | (p_data[2] << 8) | (p_data[3] << 16) | ((uint32_t)p_data[4] << 24);
            cnt  = p_data[5];
            if (cnt == 0 || cnt > PEEK_MAX || !peek_allowed(addr, cnt))
            {
                goto error;
            }
            r[0] = CMD_PEEK;
            memcpy(&r[1], &p_data[1], 4);
            peek(addr, &r[5], cnt);
            n = 5 + cnt;
            break;
        }

        case CMD_SENSE:
        {
            uint32_t pin = len >= 3 ? p_data[1] : 32;
            uint32_t sense = len >= 3 ? p_data[2] : 1;

            /* Только поле SENSE: направление и подтяжка пина остаются как были. */
            if (!probe_pin_allowed(pin) || (sense != 0 && sense != 2 && sense != 3))
            {
                goto error;
            }
            NRF_GPIO->PIN_CNF[pin] = (NRF_GPIO->PIN_CNF[pin] & ~GPIO_PIN_CNF_SENSE_Msk)
                                   | (sense << GPIO_PIN_CNF_SENSE_Pos);
            NRF_GPIOTE->EVENTS_PORT = 0;
            r[0] = CMD_SENSE;
            r[1] = (uint8_t)pin;
            r[2] = (uint8_t)sense;
            n = 3;
            break;
        }

        case CMD_SENSE_POLL:
        {
            uint32_t in = NRF_GPIO->IN;

            r[0] = CMD_SENSE_POLL;
            r[1] = (uint8_t)(NRF_GPIOTE->EVENTS_PORT != 0);
            memcpy(&r[2], &in, 4);
            NRF_GPIOTE->EVENTS_PORT = 0;
            n = 6;
            break;
        }

        case CMD_PIN:
        {
            uint32_t in;

            if (len < 3 || !probe_pin(p_data[1], p_data[2]))
            {
                goto error;
            }
            nrf_delay_us(50); /* дать входу установиться после смены подтяжки */
            in = NRF_GPIO->IN;
            r[0] = CMD_PIN;
            r[1] = p_data[1];
            r[2] = p_data[2];
            memcpy(&r[3], &in, 4);
            n = 7;
            break;
        }

        case CMD_LCD_WRITE:
            if (len < 5 || !probe_write(p_data[1], p_data[2], p_data[3], &p_data[4], len - 4u))
            {
                goto error;
            }
            r[0] = CMD_LCD_WRITE;
            r[1] = (uint8_t)(len - 4);
            n = 2;
            break;

        case CMD_LCD_FILL:
            if (len < 9 || !probe_fill(p_data[1], p_data[2], p_data[3],
                                       (uint16_t)(p_data[4] | (p_data[5] << 8)),
                                       p_data[6] | (p_data[7] << 8) | ((uint32_t)p_data[8] << 16)))
            {
                goto error;
            }
            r[0] = CMD_LCD_FILL;
            n = 1;
            break;

        case CMD_LCD_READ:
            if (len < 6 || !probe_read(p_data[1], p_data[2], p_data[3], p_data[4], p_data[5], &r[2]))
            {
                goto error;
            }
            r[0] = CMD_LCD_READ;
            r[1] = p_data[4];
            n = (uint16_t)(2 + p_data[5]);
            break;

        default:
            goto error;
    }
    respond(r, n);
    return;

error:
    r[0] = RESP_ERROR;
    r[1] = p_data[0];
    respond(r, 2);
}


/* Число в строку с ведущими нулями до width знаков. Возвращает конец строки. */
static char * fmt_uint(char * p, uint32_t value, int width)
{
    char tmp[10];
    int  n = 0;

    do
    {
        tmp[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && n < (int)sizeof(tmp));
    while (n < width)
    {
        tmp[n++] = '0';
    }
    while (n)
    {
        *p++ = tmp[--n];
    }
    *p = 0;
    return p;
}

static void text_pin(int x, int y, uint32_t pin)
{
    char   buf[6];
    char * p = fmt_uint(buf, pin, 2);

    *p++ = ':';
    *p++ = (char)('0' + ((m_inputs >> pin) & 1));
    *p   = 0;
    display_text(x, y, buf, 1);
}

static void text_value(int y, const char * p_label, uint32_t value)
{
    char buf[12];

    fmt_uint(buf, value, 4);
    display_text(2, y, p_label, 1);
    display_text(26, y, buf, 1);
}

/* График последних TRACE_LEN отсчётов PPG с автомасштабом. */
static void draw_trace(int y, int h)
{
    uint16_t lo = 0xFFFF, hi = 0;

    for (int i = 0; i < TRACE_LEN; i++)
    {
        lo = m_trace[i] < lo ? m_trace[i] : lo;
        hi = m_trace[i] > hi ? m_trace[i] : hi;
    }
    display_rect(0, y - 1, DISPLAY_W, h + 2, false);
    if (hi <= lo)
    {
        return;
    }
    for (int i = 0; i < TRACE_LEN; i++)
    {
        uint16_t v = m_trace[(m_trace_pos + i) % TRACE_LEN];
        display_pixel(1 + i, y + h - 1 - (int)(v - lo) * (h - 1) / (hi - lo), true);
    }
}

static void text_center(int y, const char * p_str, int scale)
{
    int w = 6 * scale * (int)strlen(p_str);

    if (w > DISPLAY_W && scale > 1)
    {
        scale = 1; /* крупно не влезает — мелким шрифтом */
        w = 6 * (int)strlen(p_str);
    }
    display_text((DISPLAY_W - w) / 2 + 1, y, p_str, scale);
}

static const char * hid_state(void)
{
    return m_hid_err ? "HID ERR" : (hid_ready() ? "PHONE OK" : "NO PHONE");
}

static void status_draw(void)
{
    bool hr = sensors_hr_enabled();

    display_text(2, 1, "F4 " FW_VERSION, 1);
    text_pin(2, 12, 10);
    text_pin(34, 12, 11);
    text_pin(2, 21, 18);
    text_pin(34, 21, 19);
    text_pin(2, 30, 28);
    text_pin(34, 30, 29);
    text_value(42, "BAT", sensors_battery_mv());
    display_text(2, 53, hr ? "HR ON" : "HR OFF", 1);
    if (NRF_WDT->RUNSTATUS)
    {
        display_text(DISPLAY_W - 19, 53, "WDT", 1);
    }
    if (!hr)
    {
        /* Причина последнего старта: TOUCH/CHARGE/WDT после сна, BOOT — без сна. */
        static const char hex[] = "0123456789ABCDEF";
        char buf[6];

        display_text(2, 62, "WAKE", 1);
        display_text(32, 62, m_wake, 1);
        for (int i = 0; i < 5; i++)
        {
            buf[i] = hex[(m_resetreas >> (4 * (4 - i))) & 0xF];
        }
        buf[5] = 0;
        display_text(2, 71, "RST", 1);
        display_text(26, 71, buf, 1);
    }
    if (hr)
    {
        text_value(62, "P01", m_trace[(m_trace_pos + TRACE_LEN - 1) % TRACE_LEN]);
        text_value(71, "P03", sensors_hr_aux_raw());
        draw_trace(84, 26);
    }
    {
        const char * p_hid = m_hid_err ? "HID ERR" : (hid_ready() ? "HID READY" : "HID --");
        display_text((DISPLAY_W - 6 * (int)strlen(p_hid)) / 2, DISPLAY_H - 19, p_hid, 1);
    }
    display_text((DISPLAY_W - 6 * (int)strlen(m_status)) / 2, DISPLAY_H - 9, m_status, 1);
}

/* Страница меню: номер, заголовок, что делает долгое нажатие. */
static void page_draw(void)
{
    char buf[4] = { (char)('1' + m_page), '/', (char)('0' + PAGE_COUNT), 0 };

    display_text(DISPLAY_W - 18, 1, buf, 1);
    text_center(16, m_page_title[m_page], 2);
    display_rect(4, 34, DISPLAY_W - 8, 1, true);
    switch (m_page)
    {
        case PAGE_CAMERA:
            text_center(50, "HOLD:", 1);
            text_center(60, "SHUTTER", 1);
            text_center(80, hid_state(), 1);
            break;

        case PAGE_SCRIPTS:
            text_center(50, "KEYBOARD", 1);
            text_center(60, "MACROS", 1);
            text_center(80, "SOON", 1);
            break;

        case PAGE_MOUSE:
            text_center(50, "TILT TO", 1);
            text_center(60, "MOVE", 1);
            text_center(80, "SOON", 1);
            break;

        case PAGE_POWER:
            text_center(50, "HOLD:", 1);
            text_center(60, "DEEP SLEEP", 1);
            text_center(80, "WAKE: TAP", 1);
            text_center(90, "OR CHARGER", 1);
            break;
    }
    text_center(DISPLAY_H - 9, "TAP: NEXT", 1);
}

static void screen_draw(void)
{
    if (!m_screen_on || m_image_ticks)
    {
        return; /* погашенный экран не перерисовываем; картинку не затираем */
    }
    display_clear();
    if (m_page == PAGE_STATUS)
    {
        status_draw();
    }
    else
    {
        page_draw();
    }
    if (m_note_ticks)
    {
        display_rect(0, DISPLAY_H - 33, DISPLAY_W, 13, false);
        text_center(DISPLAY_H - 30, m_note, 1);
    }
    display_flush();
}

/* Продлить показ; погашенный экран будит: сначала кадр, потом подсветка. */
static void screen_wake(void)
{
    m_screen_left = SCREEN_ON_TICKS;
    if (!m_screen_on)
    {
        display_sleep(false);
        m_screen_on = true;
        screen_draw();
        display_backlight(true);
    }
}

/* Картинка или анимация с внешней флеш по заголовку на IMAGE_ADDR ("IMG1"/"ANI1"). */
static bool image_show(void)
{
    uint8_t  hdr[12];
    uint32_t w, h, n = 1, ms = 0, data;

    extflash_read(IMAGE_ADDR, hdr, sizeof(hdr));
    w = hdr[4] | (hdr[5] << 8);
    h = hdr[6] | (hdr[7] << 8);
    if (memcmp(hdr, "IMG1", 4) == 0)
    {
        data = IMAGE_ADDR + 8;
    }
    else if (memcmp(hdr, "ANI1", 4) == 0)
    {
        n    = hdr[8] | (hdr[9] << 8);
        ms   = hdr[10] | (hdr[11] << 8);
        data = IMAGE_ADDR + 12;
    }
    else
    {
        return false;
    }
    if (w == 0 || h == 0 || w > DISPLAY_W || h > DISPLAY_H || n == 0
        || data + n * w * h * 2 > EXTFLASH_USER_END)
    {
        return false;
    }
    if (w < DISPLAY_W || h < DISPLAY_H)
    {
        display_clear(); /* поля вокруг маленькой картинки — чёрные */
        display_flush();
    }
    m_anim_base   = data;
    m_anim_size   = w * h * 2;
    m_anim_w      = (uint16_t)w;
    m_anim_h      = (uint16_t)h;
    m_anim_count  = (uint16_t)n;
    m_anim_frame  = 0;
    m_anim_tick   = 0;
    m_anim_period = (uint8_t)((ms * TICK_HZ + 500) / 1000);
    if (m_anim_period == 0)
    {
        m_anim_period = 1;
    }
    return display_image_flash(data, w, h);
}

/* Следующий кадр анимации, если пора (вызывается каждый тик RTC1). */
static void anim_step(void)
{
    if (m_anim_count < 2 || ++m_anim_tick < m_anim_period)
    {
        return;
    }
    m_anim_tick  = 0;
    m_anim_frame = (uint16_t)((m_anim_frame + 1) % m_anim_count);
    display_image_flash(m_anim_base + m_anim_frame * m_anim_size, m_anim_w, m_anim_h);
}

static void screen_sleep(void)
{
#if DISPLAY_SH1107
    return; /* OLED: сна нет, P0.30 там питание, а не подсветка */
#endif
    m_image_ticks = 0;
    m_note_ticks = 0;
    m_page = PAGE_STATUS; /* следующее включение — с состояния устройства */
    if (m_screen_on)
    {
        m_screen_on = false;
        display_sleep(true);
    }
}

/* Мотор сразу на полный ход; выключит счётчик m_motor_ticks в RTC1_IRQHandler. */
static void buzz(uint16_t ticks)
{
    if (m_motor_ticks == 0)
    {
        sensors_motor(true);
    }
    if (m_motor_ticks < ticks)
    {
        m_motor_ticks = ticks;
    }
}

static void note(const char * p_text)
{
    m_note = p_text;
    m_note_ticks = NOTE_TICKS;
}

/* Сон до касания или зарядки; выход — только сбросом.
 * Зовётся из RTC1_IRQHandler; назад не возвращается. */
static void deep_sleep(void)
{
    uint32_t wake_mask = SENSORS_WAKE_MASK;

#if !DISPLAY_SH1107
    display_sleep(true); /* подсветка, DISPOFF, SLPIN */
#endif
    extflash_sleep();
    NRF_SPI0->ENABLE = 0;
    sensors_sleep();

    sd_softdevice_disable(); /* связь рвётся; дальше периферия и NVIC — напрямую */
    NVIC->ICER[0] = 0xFFFFFFFF;
    NVIC->ICPR[0] = 0xFFFFFFFF;
    NRF_RTC1->TASKS_STOP = 1;
    NRF_RTC1->INTENCLR = 0xFFFFFFFF;
    NRF_CLOCK->TASKS_HFCLKSTOP = 1; /* кварц 16 МГц, если SoftDevice его оставил */

    NRF_POWER->RESETREAS = 0xFFFFFFFF; /* следующий старт покажет только свою причину */
    NRF_POWER->GPREGRET  = SLEEP_MAGIC; /* System OFF его сохраняет */

    if (!NRF_WDT->RUNSTATUS)
    {
        /* System OFF: выход — сброс по DETECT (RESETREAS.OFF). Если касание или
         * зарядка уже есть, DETECT поднят, и чип сразу же проснётся. */
        __DSB();
        NRF_POWER->SYSTEMOFF = 1;
        for (;;)
        {
        }
    }

    /* WDT остался от сна 0.12.x (остановить его может только сброс от него же
     * или по питанию): System ON + WFE, он разбудит сам, когда истечёт.
     * PORT при запрещённом в NVIC прерывании только ставит его в ожидание, а
     * SEVONPEND превращает это в событие для WFE. */
    NRF_GPIOTE->EVENTS_PORT = 0;
    NRF_GPIOTE->INTENSET = GPIOTE_INTENSET_PORT_Msk;
    SCB->SCR |= SCB_SCR_SEVONPEND_Msk;
    while (NRF_GPIOTE->EVENTS_PORT == 0 && (NRF_GPIO->IN & wake_mask) == wake_mask)
    {
        __WFE();
    }
    NVIC_SystemReset();
}

/* Долгое нажатие: действие текущей страницы. */
static void page_action(void)
{
    switch (m_page)
    {
        case PAGE_STATUS:
            if (image_show())
            {
                m_image_ticks = IMAGE_TICKS;
                return;
            }
            note("NO IMAGE");
            break;

        case PAGE_CAMERA:
            note(hid_volume_up_click() ? "SHOT" : hid_state());
            break;

        case PAGE_POWER:
            if (sensors_charging())
            {
                note("CHARGING"); /* P0.29 = 0 разбудил бы сразу же */
                break;
            }
            m_sleep_armed = true;
            note("RELEASE");
            break;

        default:
            note("SOON");
            break;
    }
    screen_draw();
}

/* Сенсорная кнопка, каждый тик RTC1. */
static void touch_tick(bool touched)
{
    if (!touched)
    {
        if (m_press_ticks && m_sleep_armed)
        {
            m_sleep_armed = false;
            m_sleep_ticks = SLEEP_DELAY_TICKS; /* дальше — RTC1_IRQHandler */
            note("SLEEP");
            screen_draw();
        }
        else if (m_press_ticks && !m_press_used)
        {
            m_page = (uint8_t)((m_page + 1) % PAGE_COUNT);
            m_note_ticks = 0;
            screen_draw();
        }
        m_press_ticks = 0;
        return;
    }

    if (m_press_ticks == 0)
    {
        buzz(TOUCH_BUZZ_TICKS);
        m_press_used = false;
        if (m_sleep_ticks)
        {
            m_sleep_ticks = 0; /* передумали засыпать */
            m_press_used = true;
            note("CANCELED");
            screen_draw();
        }
        else if (!m_screen_on)
        {
            m_press_used = true; /* только разбудить: без листания и действия */
            screen_wake();
        }
        else if (m_image_ticks)
        {
            m_press_used = true; /* касание убирает картинку */
            m_image_ticks = 0;
            screen_draw();
        }
    }
    m_screen_left = SCREEN_ON_TICKS; /* пока держат, экран не гаснет */
    if (m_press_ticks < 255)
    {
        m_press_ticks++;
    }
    if (!m_press_used && m_press_ticks == LONG_PRESS_TICKS)
    {
        m_press_used = true;
        buzz(LONG_BUZZ_TICKS);
        page_action();
    }
}

static void set_status(const char * p_status)
{
    m_status = p_status;
    screen_draw();
}

/* RTC1, TICK_HZ: опрос входов, отсчёты PPG, экран. Приоритет как у SWI2 — не вытесняют друг друга. */
void RTC1_IRQHandler(void)
{
    uint32_t inputs;
    bool     redraw = false;

    NRF_RTC1->EVENTS_TICK = 0;

    if (NRF_WDT->RUNSTATUS)
    {
        NRF_WDT->RR[0] = WDT_RR_RR_Reload; /* WDT сна пережил программный сброс */
    }
    if (m_sleep_ticks && --m_sleep_ticks == 0)
    {
        deep_sleep();
    }

    inputs = sensors_inputs();
    if (inputs != m_inputs)
    {
        uint8_t r[5] = { EVT_INPUTS };

        m_inputs = inputs;
        memcpy(&r[1], &inputs, 4);
        respond(r, sizeof(r));
        redraw = m_page == PAGE_STATUS;
    }
    touch_tick(!(inputs & (1u << PIN_TOUCH)));

    if (sensors_hr_enabled())
    {
        uint16_t v = sensors_hr_raw();

        m_trace[m_trace_pos] = v;
        m_trace_pos = (m_trace_pos + 1) % TRACE_LEN;
        m_ppg[m_ppg_count++] = v;
        if (m_ppg_count == PPG_PER_PACKET)
        {
            uint8_t r[RESP_MAX] = { EVT_PPG, m_ppg_seq++ };

            memcpy(&r[2], m_ppg, sizeof(m_ppg));
            respond(r, sizeof(r));
            m_ppg_count = 0;
        }
    }

    if (m_motor_ticks && --m_motor_ticks == 0)
    {
        sensors_motor(false);
    }

    if (m_image_ticks)
    {
        if (--m_image_ticks == 0)
        {
            redraw = true; /* картинка показана — вернуть обычный экран */
        }
        else
        {
            anim_step();
        }
    }

    if (m_note_ticks && --m_note_ticks == 0)
    {
        redraw = true;
    }

    if (m_screen_on && --m_screen_left == 0)
    {
        screen_sleep();
    }

    if (++m_tick >= SCREEN_EVERY)
    {
        m_tick = 0;
        redraw = true;
    }
    if (redraw)
    {
        screen_draw();
    }
}

static void rtc_start(void)
{
    NRF_RTC1->PRESCALER = RTC_PRESCALER;
    NRF_RTC1->EVENTS_TICK = 0;
    NRF_RTC1->INTENSET = RTC_INTENSET_TICK_Msk;
    CHECK(sd_nvic_SetPriority(RTC1_IRQn, NRF_APP_PRIORITY_LOW));
    CHECK(sd_nvic_EnableIRQ(RTC1_IRQn));
    NRF_RTC1->TASKS_START = 1;
}

static void adv_start(void)
{
    ble_gap_adv_params_t params;

    memset(&params, 0, sizeof(params));
    params.type     = BLE_GAP_ADV_TYPE_ADV_IND;
    params.fp       = BLE_GAP_ADV_FP_ANY;
    params.interval = ADV_INTERVAL;
    params.timeout  = 0;
    CHECK(sd_ble_gap_adv_start(&params));
}

static void on_ble_evt(const ble_evt_t * p_evt)
{
    hid_on_ble_evt(p_evt); /* сопряжение, ключи, CCCD, отложенное «отпустить» */
    switch (p_evt->header.evt_id)
    {
        case BLE_GAP_EVT_CONNECTED:
            m_conn = p_evt->evt.gap_evt.conn_handle;
            screen_wake();
            set_status("CONNECTED");
            break;

        case BLE_GAP_EVT_DISCONNECTED:
            m_conn = BLE_CONN_HANDLE_INVALID;
            stream_stop();
            motor_stop();
            if (m_dfu_pending)
            {
                enter_dfu();
            }
            set_status("ADV");
            adv_start();
            break;

        case BLE_EVT_TX_COMPLETE:
            stream_pump();
            break;

        case BLE_GATTS_EVT_WRITE:
        {
            const ble_gatts_evt_write_t * p_write = &p_evt->evt.gatts_evt.params.write;
            if (p_write->handle == m_ctrl.value_handle)
            {
                on_command(p_write->data, p_write->len);
            }
            break;
        }

        default:
            break;
    }
}

/* Прерывание событий SoftDevice (SWI2). */
void SWI2_IRQHandler(void)
{
    uint32_t soc_evt;

    while (sd_evt_get(&soc_evt) == NRF_SUCCESS)
    {
    }
    for (;;)
    {
        uint16_t len = sizeof(m_evt_buf);
        if (sd_ble_evt_get((uint8_t *)m_evt_buf, &len) != NRF_SUCCESS)
        {
            break;
        }
        on_ble_evt((const ble_evt_t *)m_evt_buf);
    }
}


static void char_add(uint16_t service, uint8_t uuid_type, uint16_t uuid, bool is_ctrl,
                     ble_gatts_char_handles_t * p_handles)
{
    ble_gatts_char_md_t char_md;
    ble_gatts_attr_md_t cccd_md;
    ble_gatts_attr_md_t attr_md;
    ble_gatts_attr_t    attr;
    ble_uuid_t          char_uuid = { .uuid = uuid, .type = uuid_type };

    memset(&char_md, 0, sizeof(char_md));
    memset(&cccd_md, 0, sizeof(cccd_md));
    memset(&attr_md, 0, sizeof(attr_md));
    memset(&attr, 0, sizeof(attr));

    if (is_ctrl)
    {
        char_md.char_props.write         = 1;
        char_md.char_props.write_wo_resp = 1;
    }
    else
    {
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.read_perm);
        BLE_GAP_CONN_SEC_MODE_SET_OPEN(&cccd_md.write_perm);
        cccd_md.vloc = BLE_GATTS_VLOC_STACK;

        char_md.char_props.read   = 1;
        char_md.char_props.notify = 1;
        char_md.p_cccd_md         = &cccd_md;
    }

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.read_perm);
    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&attr_md.write_perm);
    attr_md.vloc = BLE_GATTS_VLOC_STACK;
    attr_md.vlen = 1;

    attr.p_uuid    = &char_uuid;
    attr.p_attr_md = &attr_md;
    attr.init_len  = 1;
    attr.max_len   = RESP_MAX;

    CHECK(sd_ble_gatts_characteristic_add(service, &char_md, &attr, p_handles));
}

static void ble_init(void)
{
    ble_enable_params_t     enable;
    ble_gap_conn_sec_mode_t open;
    ble_gap_conn_params_t   conn_params;
    ble_uuid_t              service_uuid;
    uint16_t                service;
    uint8_t                 uuid_type;

    memset(&enable, 0, sizeof(enable));
    enable.gatts_enable_params.service_changed = 1;
    enable.gatts_enable_params.attr_tab_size   = BLE_GATTS_ATTR_TAB_SIZE_DEFAULT;
    CHECK(sd_ble_enable(&enable));

    BLE_GAP_CONN_SEC_MODE_SET_OPEN(&open);
    CHECK(sd_ble_gap_device_name_set(&open, (const uint8_t *)DEVICE_NAME, strlen(DEVICE_NAME)));

    memset(&conn_params, 0, sizeof(conn_params));
    conn_params.min_conn_interval = 16;  /* 20 мс  */
    conn_params.max_conn_interval = 60;  /* 75 мс  */
    conn_params.slave_latency     = 0;
    conn_params.conn_sup_timeout  = 400; /* 4 с    */
    CHECK(sd_ble_gap_ppcp_set(&conn_params));

    CHECK(sd_ble_uuid_vs_add(&m_base_uuid, &uuid_type));
    service_uuid.uuid = UUID_SERVICE;
    service_uuid.type = uuid_type;
    CHECK(sd_ble_gatts_service_add(BLE_GATTS_SRVC_TYPE_PRIMARY, &service_uuid, &service));
    char_add(service, uuid_type, UUID_CTRL, true, &m_ctrl);
    char_add(service, uuid_type, UUID_RESP, false, &m_resp);

    /* HID не через CHECK: при ошибке работаем без него, а не уходим в DFU на каждом старте. */
    m_hid_err = hid_init();
    if (m_hid_err == NRF_SUCCESS)
    {
        sd_ble_gap_appearance_set(BLE_APPEARANCE_GENERIC_HID);
    }
}

/* Реклама: флаги, внешний вид HID, UUID 0x1812 (по ним телефон узнаёт клавиатуру)
 * и 128-битный UUID нашего сервиса (по нему ищет enter_dfu.py); имя — в scan response. */
static void adv_data_set(void)
{
    uint8_t adv[3 + 4 + 4 + 2 + 16];
    uint8_t k = 0;
    uint8_t sr[2 + sizeof(DEVICE_NAME) - 1];

    adv[k++] = 2;
    adv[k++] = 0x01; /* Flags */
    adv[k++] = 0x06; /* LE General Discoverable, BR/EDR not supported */
    if (m_hid_err == NRF_SUCCESS)
    {
        adv[k++] = 3;
        adv[k++] = 0x19; /* Appearance */
        adv[k++] = (uint8_t)(BLE_APPEARANCE_GENERIC_HID & 0xFF);
        adv[k++] = (uint8_t)(BLE_APPEARANCE_GENERIC_HID >> 8);
        adv[k++] = 3;
        adv[k++] = 0x03; /* Complete list of 16-bit UUIDs */
        adv[k++] = 0x12; /* HID 0x1812 */
        adv[k++] = 0x18;
    }
    adv[k++] = 17;
    adv[k++] = 0x07; /* Complete list of 128-bit UUIDs */
    memcpy(&adv[k], m_base_uuid.uuid128, 16);
    adv[k + 12] = (uint8_t)(UUID_SERVICE & 0xFF);
    adv[k + 13] = (uint8_t)(UUID_SERVICE >> 8);
    k += 16;

    sr[0] = sizeof(sr) - 1;
    sr[1] = 0x09; /* Complete local name */
    memcpy(&sr[2], DEVICE_NAME, sizeof(DEVICE_NAME) - 1);

    CHECK(sd_ble_gap_adv_data_set(adv, k, sr, sizeof(sr)));
}


int main(void);

/* Вызывается из gcc_startup_nrf51.s вместо crt0 из newlib (сборка с -nostartfiles). */
void _start(void)
{
    extern uint32_t __bss_start__, __bss_end__;

    for (uint32_t * p = &__bss_start__; p < &__bss_end__; p++)
    {
        *p = 0;
    }
    main();
    for (;;)
    {
    }
}

int main(void)
{
    bool woke = false;

    if (NRF_POWER->GPREGRET == APP_DFU_REQUEST)
    {
        NRF_POWER->GPREGRET = 0;
        enter_dfu();
    }

    if (NRF_WDT->RUNSTATUS)
    {
        NRF_WDT->RR[0] = WDT_RR_RR_Reload;
    }
    /* Загрузчик RESETREAS не очищает (флаг DOG жил там с заводской прошивки). */
    m_resetreas = NRF_POWER->RESETREAS;
    NRF_POWER->RESETREAS = m_resetreas;

    sensors_init();
    nrf_delay_us(20); /* подтяжка P0.29 */
    /* Сброс от WDT стирает GPREGRET [проверено 2026-11-19], поэтому DOG — сам по
     * себе признак (WDT запускал только сон 0.12.0–0.12.1). */
    if (m_resetreas & POWER_RESETREAS_DOG_Msk)
    {
        m_wake = "WDT";
        woke = true;
    }
    else if ((m_resetreas & POWER_RESETREAS_OFF_Msk) || NRF_POWER->GPREGRET == SLEEP_MAGIC)
    {
        m_wake = sensors_charging() ? "CHARGE" : "TOUCH";
        woke = true;
    }
    if (NRF_POWER->GPREGRET == SLEEP_MAGIC)
    {
        NRF_POWER->GPREGRET = 0;
    }
    m_inputs = sensors_inputs();
    if (!(m_inputs & (1u << PIN_TOUCH)))
    {
        /* Палец ещё на кнопке после пробуждения: это касание не листает и не действует. */
        m_press_ticks = 1;
        m_press_used = true;
    }
    display_init();
    extflash_wake(); /* после сна флеш остаётся в deep power-down и после сброса */
    screen_draw();

    CHECK(sd_softdevice_enable(LFCLK_SRC, sd_assert));
    ble_init();
    adv_data_set();
    adv_start();

    CHECK(sd_nvic_SetPriority(SD_EVT_IRQn, NRF_APP_PRIORITY_LOW));
    CHECK(sd_nvic_EnableIRQ(SD_EVT_IRQn));
    rtc_start(); /* LFCLK уже запущен SoftDevice */
    if (woke)
    {
        buzz(WAKE_BUZZ_TICKS); /* проснулись из сна */
    }

    for (;;)
    {
        sd_app_evt_wait();
    }
}
