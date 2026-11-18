/* OLED 64x128 на SPI0 — повтор заводского драйвера.
 *
 * Пины и порядок взяты из заводской прошивки:
 *   0x299AC  выходы P0.24 (CS)=1, P0.23 (RES)=1, P0.22 (D/C), P0.30=1, P0.21=1;
 *   0x2C7C0  SPI0: SCK P0.04, MOSI P0.13, MISO P0.06, 4 МГц, режим 0, MSB first;
 *            P0.05 — выход в 1;
 *   0x299F8  байт: D/C = dc, CS=0, SPI, CS=1;
 *   0x2B368  сброс 1/0/1 (100/100/200 мс), 26 команд из 0x30A00,
 *            8D 10 AE, вывод буфера (0x2B418), 8D 14 AF.
 * Буфер: страница y/8, столбец x, бит y%8. Это заводской setpixel (0x2B318)
 * с y' = 127 - y. Так верх экрана совпадает с верхом PNG из f4gfx.py: по
 * блиттеру 0x2B278 выходит, что строки картинки идут по x, а биты — от
 * большего y к меньшему. На железе не проверено: если изображение
 * перевёрнуто, менять здесь.
 */
#include "display.h"

#include <string.h>

#include "nrf.h"
#include "nrf_delay.h"
#include "font5x7.h"
#include "extflash.h"

#define PIN_SCK   4
#define PIN_MISO  6
#define PIN_MOSI  13
#define PIN_PWR1  21
#define PIN_DC    22
#define PIN_RES   23
#define PIN_CS    24
#define PIN_PWR2  30  /* цветная версия: подсветка TFT (1 — горит) */
#define PIN_AUX   5
/* Другие устройства на SPI0: без CS = 1 они слушают трафик дисплея. */
#define PIN_CS_DEV17  17  /* CS неизвестного устройства (0x1FCC4) */
#define PIN_CS_FLASH  25  /* CS внешней SPI-флеш (0x2C462, 0x1AF5C) */

#define SPI_TIMEOUT 0x3000 /* как в заводском 0x2C884 */

/* Цветная версия (DISPLAY_SH1107 = 0), найдено зондом 0.5.0 на железе, 2026-11-18:
 * TFT 80x160 на контроллере ST7735S, те же пины CS/DC/RES. Память контроллера
 * 132x162, видимое окно — столбцы 26..105, строки 1..160. MADCTL 0xC8
 * (MY и MX — иначе картинка перевёрнута и отзеркалена, BGR — иначе красный
 * и синий меняются; сдвиг столбцов при MX тот же: 132 - 80 - 26 = 26),
 * инверсия включена (INVON). Подсветка — P0.30, активный уровень 1 (проверено
 * на железе: P0.30 = 0 гасит экран, P0.21 = 0 видимого эффекта не даёт).
 * Чтения у контроллера нет (линия не разведена). */
#define TFT_X0     26
#define TFT_Y0     1
#define TFT_MADCTL 0xC8

#define PIN_XL1   26  /* выводы кварца 32 кГц: не трогать */
#define PIN_XL2   27

static const uint8_t m_init_cmds[] = {
    0xAE, 0x00, 0x10, 0xB0, 0xDC, 0x00, 0x81, 0x80, 0x20, 0xA0, 0xC1, 0xA4, 0xA6,
    0xA8, 0x3F, 0xD3, 0x60, 0xD5, 0x51, 0xD9, 0x22, 0xDB, 0x35, 0xAD, 0x8A, 0xAF,
};

static uint8_t m_fb[DISPLAY_H / 8][DISPLAY_W];
static uint16_t m_fg = 0xFFFF;
static uint16_t m_bg = 0x0000;


static void pin_output(uint32_t pin, bool high)
{
    if (high)
    {
        NRF_GPIO->OUTSET = 1u << pin;
    }
    else
    {
        NRF_GPIO->OUTCLR = 1u << pin;
    }
    NRF_GPIO->PIN_CNF[pin] = (GPIO_PIN_CNF_DIR_Output << GPIO_PIN_CNF_DIR_Pos)
                           | (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos);
}

static void pin_write(uint32_t pin, bool high)
{
    if (high)
    {
        NRF_GPIO->OUTSET = 1u << pin;
    }
    else
    {
        NRF_GPIO->OUTCLR = 1u << pin;
    }
}

static void spi_init(void)
{
    pin_output(PIN_SCK, false);
    pin_output(PIN_MOSI, false);
    NRF_GPIO->PIN_CNF[PIN_MISO] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos)
                                | (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos)
                                | (GPIO_PIN_CNF_PULL_Pullup << GPIO_PIN_CNF_PULL_Pos);
    pin_output(PIN_AUX, true);

    NRF_SPI0->PSELSCK   = PIN_SCK;
    NRF_SPI0->PSELMOSI  = PIN_MOSI;
    NRF_SPI0->PSELMISO  = PIN_MISO;
#if DISPLAY_SH1107
    NRF_SPI0->FREQUENCY = SPI_FREQUENCY_FREQUENCY_M4;
#else
    NRF_SPI0->FREQUENCY = SPI_FREQUENCY_FREQUENCY_M8; /* ST7735S пишет до 15 МГц */
#endif
    NRF_SPI0->CONFIG    = 0; /* MSB first, CPHA=0, CPOL=0 */
    NRF_SPI0->EVENTS_READY = 0;
    NRF_SPI0->ENABLE    = SPI_ENABLE_ENABLE_Enabled << SPI_ENABLE_ENABLE_Pos;
}

static void spi_tx(uint8_t byte)
{
    uint32_t n = 0;

    NRF_SPI0->TXD = byte;
    while (NRF_SPI0->EVENTS_READY == 0 && ++n < SPI_TIMEOUT)
    {
    }
    NRF_SPI0->EVENTS_READY = 0;
    (void)NRF_SPI0->RXD;
}

static void write_byte(bool is_data, uint8_t byte)
{
    pin_write(PIN_DC, is_data);
    pin_write(PIN_CS, false);
    spi_tx(byte);
    pin_write(PIN_CS, true);
}

static void cmd(uint8_t byte)
{
    write_byte(false, byte);
}


/* Команда и параметры одной посылкой (CS держим всю посылку). */
static void tft_cmd(uint8_t c, const uint8_t * p_args, uint32_t n)
{
    pin_write(PIN_CS, false);
    pin_write(PIN_DC, false);
    spi_tx(c);
    pin_write(PIN_DC, true);
    while (n--)
    {
        spi_tx(*p_args++);
    }
    pin_write(PIN_CS, true);
}

/* Окно в координатах экрана и RAMWR; после этого D/C = 1, идут пиксели. */
static void tft_window(uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
{
    uint8_t ca[4] = { 0, (uint8_t)(x0 + TFT_X0), 0, (uint8_t)(x1 + TFT_X0) };
    uint8_t ra[4] = { 0, (uint8_t)(y0 + TFT_Y0), 0, (uint8_t)(y1 + TFT_Y0) };

    tft_cmd(0x2A, ca, 4);   /* CASET */
    tft_cmd(0x2B, ra, 4);   /* RASET */
    tft_cmd(0x2C, NULL, 0); /* RAMWR, данные — следующей посылкой с D/C = 1 */
    pin_write(PIN_DC, true);
}

/* Питание матрицы и гамма — типовая инициализация модулей 0,96" 80x160 IPS
 * на ST7735S. С ними картинка светлее и контрастнее, чем со значениями после
 * сброса (сравнено на часах 2026-11-18). Формат: команда, число параметров, параметры. */
static const uint8_t m_tft_tune[] = {
    0xB1, 3, 0x01, 0x2C, 0x2D,                         /* FRMCTR1: частота кадров */
    0xB2, 3, 0x01, 0x2C, 0x2D,                         /* FRMCTR2 */
    0xB3, 6, 0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D,       /* FRMCTR3 */
    0xB4, 1, 0x07,                                     /* INVCTR */
    0xC0, 3, 0xA2, 0x02, 0x84,                         /* PWCTR1 */
    0xC1, 1, 0xC5,                                     /* PWCTR2 */
    0xC2, 2, 0x0A, 0x00,                               /* PWCTR3 */
    0xC3, 2, 0x8A, 0x2A,                               /* PWCTR4 */
    0xC4, 2, 0x8A, 0xEE,                               /* PWCTR5 */
    0xC5, 1, 0x0E,                                     /* VMCTR1: VCOM */
    0xE0, 16, 0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,   /* гамма + */
              0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10,
    0xE1, 16, 0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,   /* гамма − */
              0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10,
};

/* Последовательность, проверенная зондом 0.5.0 на железе. */
static void tft_init(void)
{
    static const uint8_t colmod = 0x55;       /* 16 бит на пиксель */
    static const uint8_t madctl = TFT_MADCTL;

    tft_cmd(0x01, NULL, 0); /* SWRESET */
    nrf_delay_ms(150);
    tft_cmd(0x11, NULL, 0); /* SLPOUT */
    nrf_delay_ms(150);
    for (uint32_t i = 0; i < sizeof(m_tft_tune); i += 2u + m_tft_tune[i + 1])
    {
        tft_cmd(m_tft_tune[i], &m_tft_tune[i + 2], m_tft_tune[i + 1]);
    }
    tft_cmd(0x3A, &colmod, 1);
    tft_cmd(0x36, &madctl, 1);
    tft_cmd(0x21, NULL, 0); /* INVON */
    tft_cmd(0x13, NULL, 0); /* NORON */
    display_clear();
    display_flush();        /* чёрный кадр до включения: без мусора из памяти */
    tft_cmd(0x29, NULL, 0); /* DISPON */
}

void display_init(void)
{
    pin_output(PIN_CS_DEV17, true);
    pin_output(PIN_CS_FLASH, true);
    pin_output(PIN_CS, true);
    pin_output(PIN_RES, true);
    pin_output(PIN_DC, false);
    pin_output(PIN_PWR2, true);
    pin_output(PIN_PWR1, true);
    spi_init();

    pin_write(PIN_RES, true);
    nrf_delay_ms(100);
    pin_write(PIN_RES, false);
    nrf_delay_ms(100);
    pin_write(PIN_RES, true);
    nrf_delay_ms(200);

#if !DISPLAY_SH1107
    tft_init();
    return;
#endif
    for (uint32_t i = 0; i < sizeof(m_init_cmds); i++)
    {
        cmd(m_init_cmds[i]);
    }
    cmd(0x8D); cmd(0x10); cmd(0xAE);
    display_clear();
    display_flush();
    cmd(0x8D); cmd(0x14); cmd(0xAF);
}

void display_clear(void)
{
    memset(m_fb, 0, sizeof(m_fb));
}

void display_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= DISPLAY_W || y < 0 || y >= DISPLAY_H)
    {
        return;
    }
    if (on)
    {
        m_fb[y >> 3][x] |= (uint8_t)(1 << (y & 7));
    }
    else
    {
        m_fb[y >> 3][x] &= (uint8_t)~(1 << (y & 7));
    }
}

void display_rect(int x, int y, int w, int h, bool fill)
{
    for (int j = 0; j < h; j++)
    {
        for (int i = 0; i < w; i++)
        {
            if (fill || i == 0 || j == 0 || i == w - 1 || j == h - 1)
            {
                display_pixel(x + i, y + j, true);
            }
        }
    }
}

int display_text(int x, int y, const char * p_str, int scale)
{
    for (; *p_str; p_str++)
    {
        const uint8_t * p_glyph = font5x7_glyph(*p_str);
        for (int col = 0; col < 5; col++)
        {
            for (int row = 0; row < 7; row++)
            {
                if (p_glyph[col] & (1 << row))
                {
                    display_rect(x + col * scale, y + row * scale, scale, scale, true);
                }
            }
        }
        x += 6 * scale;
    }
    return x;
}

void display_backlight(bool on)
{
    pin_write(PIN_PWR2, on);
}

void display_sleep(bool sleep)
{
#if !DISPLAY_SH1107
    if (sleep)
    {
        display_backlight(false);
        tft_cmd(0x28, NULL, 0); /* DISPOFF */
        tft_cmd(0x10, NULL, 0); /* SLPIN: ток контроллера — единицы мкА */
    }
    else
    {
        tft_cmd(0x11, NULL, 0); /* SLPOUT */
        nrf_delay_ms(120);      /* ST7735S: до следующей команды 120 мс */
        tft_cmd(0x29, NULL, 0); /* DISPON; подсветку включает вызывающий после кадра */
    }
#else
    (void)sleep;
#endif
}

bool display_image_flash(uint32_t addr, uint32_t w, uint32_t h)
{
#if !DISPLAY_SH1107
    uint8_t  buf[64];
    uint32_t left = w * h * 2;

    if (w == 0 || h == 0 || w > DISPLAY_W || h > DISPLAY_H)
    {
        return false;
    }
    /* По центру экрана. */
    tft_window((DISPLAY_W - w) / 2, (DISPLAY_H - h) / 2,
               (DISPLAY_W - w) / 2 + w - 1, (DISPLAY_H - h) / 2 + h - 1);
    while (left)
    {
        uint32_t n = left < sizeof(buf) ? left : sizeof(buf);

        /* Флеш и дисплей на одной шине: CS дисплея отпущен, пока читаем.
         * Без новой команды (D/C = 0) ST7735S продолжает RAMWR с того же места. */
        extflash_read(addr, buf, n);
        pin_write(PIN_CS, false);
        for (uint32_t i = 0; i < n; i++)
        {
            spi_tx(buf[i]);
        }
        pin_write(PIN_CS, true);
        addr += n;
        left -= n;
    }
    return true;
#else
    (void)addr; (void)w; (void)h;
    return false;
#endif
}

void display_colors(uint16_t fg, uint16_t bg)
{
    m_fg = fg;
    m_bg = bg;
}

void display_flush(void)
{
#if !DISPLAY_SH1107
    tft_window(0, 0, DISPLAY_W - 1, DISPLAY_H - 1);
    pin_write(PIN_CS, false);
    for (uint32_t y = 0; y < DISPLAY_H; y++)
    {
        const uint8_t * p_row = m_fb[y >> 3];
        uint8_t         bit   = (uint8_t)(1 << (y & 7));

        for (uint32_t x = 0; x < DISPLAY_W; x++)
        {
            uint16_t c = (p_row[x] & bit) ? m_fg : m_bg;
            spi_tx((uint8_t)(c >> 8));
            spi_tx((uint8_t)c);
        }
    }
    pin_write(PIN_CS, true);
    return;
#endif
    for (uint32_t page = 0; page < DISPLAY_H / 8; page++)
    {
        cmd(0xB0 + page);
        cmd(0x00);
        cmd(0x10);
        for (uint32_t col = 0; col < DISPLAY_W; col++)
        {
            write_byte(true, m_fb[page][col]);
        }
    }
}


/* ---------- Зонд неизвестного дисплея ----------
 * Всё вызывается из обработчика команд BLE (SWI2), как и чтение внешней
 * флеш, поэтому за шину SPI0 они не борются. После каждой операции режим
 * SPI возвращается в 0 — его ждут флеш и SH1107. */

bool probe_pin_allowed(uint32_t pin)
{
    return pin < 32 && pin != PIN_SCK && pin != PIN_MOSI && pin != PIN_MISO
        && pin != PIN_CS_FLASH && pin != PIN_XL1 && pin != PIN_XL2;
}

bool probe_pin(uint32_t pin, uint8_t mode)
{
    static const uint8_t pulls[] = {
        GPIO_PIN_CNF_PULL_Disabled, GPIO_PIN_CNF_PULL_Pullup, GPIO_PIN_CNF_PULL_Pulldown,
    };

    if (!probe_pin_allowed(pin) || mode > 4)
    {
        return false;
    }
    if (mode >= 3)
    {
        pin_output(pin, mode == 4);
    }
    else
    {
        NRF_GPIO->PIN_CNF[pin] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos)
                               | (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos)
                               | ((uint32_t)pulls[mode] << GPIO_PIN_CNF_PULL_Pos);
    }
    return true;
}

/* cs и dc должны быть разрешёнными пинами; dc = 0xFF — линии D/C нет. */
static bool probe_begin(uint8_t cs, uint8_t dc, uint8_t flags)
{
    uint32_t mode = flags & 3;

    if (!probe_pin_allowed(cs) || (dc != 0xFF && (!probe_pin_allowed(dc) || dc == cs)))
    {
        return false;
    }
    if ((NRF_GPIO->DIR & (1u << cs)) == 0)
    {
        pin_output(cs, true);
    }
    if (dc != 0xFF && (NRF_GPIO->DIR & (1u << dc)) == 0)
    {
        pin_output(dc, false);
    }
    NRF_SPI0->CONFIG = ((mode & 1) << SPI_CONFIG_CPHA_Pos) | ((mode >> 1) << SPI_CONFIG_CPOL_Pos);
    pin_write(cs, false);
    return true;
}

static void probe_end(uint8_t cs)
{
    pin_write(cs, true);
    NRF_SPI0->CONFIG = 0;
}

static void probe_dc(uint8_t dc, bool is_data)
{
    if (dc != 0xFF)
    {
        pin_write(dc, is_data);
    }
}

bool probe_write(uint8_t cs, uint8_t dc, uint8_t flags, const uint8_t * p_buf, uint32_t len)
{
    uint32_t i = 0;

    if (len == 0 || !probe_begin(cs, dc, flags))
    {
        return false;
    }
    if (!(flags & PROBE_DATA_ONLY))
    {
        probe_dc(dc, false);
        spi_tx(p_buf[i++]);
    }
    probe_dc(dc, true);
    for (; i < len; i++)
    {
        spi_tx(p_buf[i]);
    }
    probe_end(cs);
    return true;
}

bool probe_fill(uint8_t cs, uint8_t dc, uint8_t flags, uint16_t color, uint32_t count)
{
    if (!probe_begin(cs, dc, flags))
    {
        return false;
    }
    probe_dc(dc, false);
    spi_tx(0x2C); /* RAMWR */
    probe_dc(dc, true);
    while (count--)
    {
        spi_tx((uint8_t)(color >> 8));
        spi_tx((uint8_t)color);
    }
    probe_end(cs);
    return true;
}

/* Программный SPI, режим 0: данные выставляем при SCK = 0, читаем по фронту. */
static void bb_delay(void)
{
    nrf_delay_us(2);
}

static void bb_tx(uint8_t byte)
{
    for (int b = 7; b >= 0; b--)
    {
        pin_write(PIN_MOSI, (byte >> b) & 1);
        bb_delay();
        pin_write(PIN_SCK, true);
        bb_delay();
        pin_write(PIN_SCK, false);
    }
}

static uint8_t bb_rx(void)
{
    uint8_t byte = 0;

    for (int b = 0; b < 8; b++)
    {
        pin_write(PIN_SCK, true);
        bb_delay();
        byte = (uint8_t)((byte << 1) | ((NRF_GPIO->IN >> PIN_MOSI) & 1));
        pin_write(PIN_SCK, false);
        bb_delay();
    }
    return byte;
}

bool probe_read(uint8_t cs, uint8_t dc, uint8_t flags, uint8_t cmd, uint8_t n, uint8_t * p_out)
{
    if (n == 0 || n > PROBE_READ_MAX || !probe_begin(cs, dc, flags & 3))
    {
        return false;
    }
    probe_dc(dc, false);
    if (!(flags & PROBE_READ_3WIRE))
    {
        spi_tx(cmd);
        probe_dc(dc, true);
        for (uint32_t i = 0; i < n; i++)
        {
            NRF_SPI0->TXD = 0;
            for (uint32_t t = 0; NRF_SPI0->EVENTS_READY == 0 && t < SPI_TIMEOUT; t++)
            {
            }
            NRF_SPI0->EVENTS_READY = 0;
            p_out[i] = (uint8_t)NRF_SPI0->RXD;
        }
        probe_end(cs);
        return true;
    }

    /* Ответ идёт по той же линии SDA (MOSI): SPI выключаем, пины — вручную. */
    NRF_SPI0->ENABLE = 0;
    pin_write(PIN_SCK, false);
    bb_tx(cmd);
    probe_dc(dc, true);
    NRF_GPIO->PIN_CNF[PIN_MOSI] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos)
                                | (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos)
                                | (GPIO_PIN_CNF_PULL_Pulldown << GPIO_PIN_CNF_PULL_Pos);
    for (uint32_t i = 0; i < n; i++)
    {
        p_out[i] = bb_rx();
    }
    pin_output(PIN_MOSI, false);
    NRF_SPI0->ENABLE = SPI_ENABLE_ENABLE_Enabled << SPI_ENABLE_ENABLE_Pos;
    probe_end(cs);
    return true;
}
