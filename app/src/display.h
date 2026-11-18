/* OLED 64x128 (SH1107-совместимый) на SPI0 — повтор заводского драйвера. */
#ifndef DISPLAY_H_INCLUDED
#define DISPLAY_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

/* 1 — монохромные F4 (OLED SH1107 64x128), 0 — цветная версия (TFT ST7735S 80x160). */
#ifndef DISPLAY_SH1107
#define DISPLAY_SH1107 0
#endif

#if DISPLAY_SH1107
#define DISPLAY_W 64
#define DISPLAY_H 128
#else
#define DISPLAY_W 80
#define DISPLAY_H 160
#endif

/* Подсветка TFT (P0.30). На OLED этот пин — питание, выключать не стоит. */
void display_backlight(bool on);
/* Сон TFT: true — подсветка, DISPOFF, SLPIN; false — SLPOUT (блокирует 120 мс)
 * и DISPON, подсветку после этого включить самому, когда кадр уже выведен. */
void display_sleep(bool sleep);
/* Картинка w x h RGB565 (big-endian, построчно) из внешней флеш с addr — по
 * центру экрана, мимо кадрового буфера. Только TFT. */
bool display_image_flash(uint32_t addr, uint32_t w, uint32_t h);

/* Цвета RGB565 для включённых и выключенных точек (только TFT). */
void display_colors(uint16_t fg, uint16_t bg);

/* Вызывать до sd_softdevice_enable: блокирующие задержки ~400 мс. */
void display_init(void);
void display_clear(void);
void display_pixel(int x, int y, bool on);
void display_rect(int x, int y, int w, int h, bool fill);
/* Шрифт 5x7, шаг 6*scale по горизонтали. Возвращает x после текста. */
int  display_text(int x, int y, const char * p_str, int scale);
void display_flush(void);

/* Зонд для неизвестного (цветного) дисплея на SPI0, см. display.c.
 * flags: биты 0–1 — режим SPI (CPOL/CPHA), бит 2 — только данные (D/C = 1 без команды). */
#define PROBE_DATA_ONLY  0x04
/* Чтение: бит 3 — 3-проводный режим (ответ по линии MOSI, программный SPI). */
#define PROBE_READ_3WIRE 0x08
#define PROBE_READ_MAX   16

bool probe_pin_allowed(uint32_t pin);
/* mode: 0 — вход, 1 — вход с подтяжкой вверх, 2 — вниз, 3 — выход 0, 4 — выход 1. */
bool probe_pin(uint32_t pin, uint8_t mode);
bool probe_write(uint8_t cs, uint8_t dc, uint8_t flags, const uint8_t * p_buf, uint32_t len);
/* RAMWR (0x2C) и count пикселей RGB565 одного цвета. */
bool probe_fill(uint8_t cs, uint8_t dc, uint8_t flags, uint16_t color, uint32_t count);
/* Команда cmd, затем n байт ответа (с первым тактом, без пропуска dummy: сдвиг разбирает ПК). */
bool probe_read(uint8_t cs, uint8_t dc, uint8_t flags, uint8_t cmd, uint8_t n, uint8_t * p_out);

#endif
