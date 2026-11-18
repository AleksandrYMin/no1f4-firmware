/* Внешняя SPI-флеш на SPI0 (CS P0.25). Шину настраивает display_init(). */
#ifndef EXTFLASH_H_INCLUDED
#define EXTFLASH_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

/* Область, куда разрешены стирание и запись. Всё остальное — заводские
 * шрифты, таблицы и иконки (копия: backup/color/extflash_2M.bin) и журнал
 * заводской прошивки с 0x1F0000. До 0x1A0000 занято, выше — было пусто. */
#define EXTFLASH_USER_START  0x1A0000u
#define EXTFLASH_USER_END    0x1F0000u
#define EXTFLASH_SECTOR      4096u
#define EXTFLASH_PAGE        256u

/* Выводит флеш из deep power-down (0xAB) и читает JEDEC ID (0x9F). */
void extflash_id(uint8_t id[3]);
/* Чтение командой 0x03, 24-битный адрес. */
void extflash_read(uint32_t addr, uint8_t * p_buf, uint32_t len);
/* Регистры статуса: SR1 (0x05) и SR2 (0x35). */
void extflash_status(uint8_t sr[2]);
/* Стирание сектора 4 КБ (0x20). Только внутри USER-области, адрес кратен 4 КБ.
 * Блокирует до конца стирания (типично 50–300 мс). false — адрес не разрешён
 * или флеш не ответила вовремя. */
bool extflash_erase_sector(uint32_t addr);
/* Запись (0x02) до len байт, не пересекая границу страницы 256 байт. */
bool extflash_program(uint32_t addr, const uint8_t * p_buf, uint32_t len);
/* Временно (до отключения питания флеш) снять защиту записи; SR после — в sr_after. */
void extflash_unprotect_volatile(uint8_t sr_after[2]);

#endif
