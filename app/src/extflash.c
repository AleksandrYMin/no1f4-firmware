/* Внешняя SPI-флеш — по заводской прошивке (0x2C462): CS P0.25, SPI0 общий с
 * дисплеем (режим 0, MSB first), чтение «03 a2 a1 a0» и дальше данные.
 * Стирание и запись — только в EXTFLASH_USER_START..END (проверка здесь,
 * а не у вызывающего, чтобы ошибка в протоколе не задела заводские данные).
 */
#include "extflash.h"

#include "nrf.h"
#include "nrf_delay.h"

#define PIN_CS           25
#define SPI_TIMEOUT      0x3000

#define CMD_READ         0x03
#define CMD_JEDEC_ID     0x9F
#define CMD_RELEASE_DPD  0xAB
#define CMD_DEEP_PD      0xB9
#define CMD_RDSR1        0x05
#define CMD_RDSR2        0x35
#define CMD_WREN         0x06
#define CMD_SECTOR_ERASE 0x20
#define CMD_PAGE_PROGRAM 0x02
#define SR1_WIP          0x01

/* Ожидание конца записи/стирания: по 100 мкс, до ~1 с (сектор у ZB25VQ16 до 300 мс). */
#define BUSY_POLL_US     100
#define BUSY_POLLS       10000


static uint8_t xfer(uint8_t byte)
{
    uint32_t n = 0;

    NRF_SPI0->TXD = byte;
    while (NRF_SPI0->EVENTS_READY == 0 && ++n < SPI_TIMEOUT)
    {
    }
    NRF_SPI0->EVENTS_READY = 0;
    return (uint8_t)NRF_SPI0->RXD;
}

static void select(void)
{
    NRF_GPIO->OUTCLR = 1u << PIN_CS;
}

static void deselect(void)
{
    NRF_GPIO->OUTSET = 1u << PIN_CS;
}


void extflash_sleep(void)
{
    select();
    xfer(CMD_DEEP_PD);
    deselect();
    nrf_delay_us(10); /* tDP у типовых флеш — до 3 мкс */
}

void extflash_wake(void)
{
    select();
    xfer(CMD_RELEASE_DPD);
    deselect();
    nrf_delay_us(50); /* tRES1 у типовых флеш — до 30 мкс */
}

void extflash_id(uint8_t id[3])
{
    extflash_wake();
    select();
    xfer(CMD_JEDEC_ID);
    for (int i = 0; i < 3; i++)
    {
        id[i] = xfer(0);
    }
    deselect();
}

void extflash_read(uint32_t addr, uint8_t * p_buf, uint32_t len)
{
    select();
    xfer(CMD_READ);
    xfer((uint8_t)(addr >> 16));
    xfer((uint8_t)(addr >> 8));
    xfer((uint8_t)addr);
    while (len--)
    {
        *p_buf++ = xfer(0);
    }
    deselect();
}

static uint8_t read_reg(uint8_t cmd)
{
    uint8_t v;

    select();
    xfer(cmd);
    v = xfer(0);
    deselect();
    return v;
}

void extflash_status(uint8_t sr[2])
{
    sr[0] = read_reg(CMD_RDSR1);
    sr[1] = read_reg(CMD_RDSR2);
}

static bool wait_ready(void)
{
    for (uint32_t i = 0; i < BUSY_POLLS; i++)
    {
        if (!(read_reg(CMD_RDSR1) & SR1_WIP))
        {
            return true;
        }
        nrf_delay_us(BUSY_POLL_US);
    }
    return false;
}

static void write_enable(void)
{
    select();
    xfer(CMD_WREN);
    deselect();
}

static void send_addr(uint32_t addr)
{
    xfer((uint8_t)(addr >> 16));
    xfer((uint8_t)(addr >> 8));
    xfer((uint8_t)addr);
}

bool extflash_erase_sector(uint32_t addr)
{
    if (addr % EXTFLASH_SECTOR || addr < EXTFLASH_USER_START || addr >= EXTFLASH_USER_END)
    {
        return false;
    }
    if (!wait_ready())
    {
        return false;
    }
    write_enable();
    select();
    xfer(CMD_SECTOR_ERASE);
    send_addr(addr);
    deselect();
    return wait_ready();
}

bool extflash_program(uint32_t addr, const uint8_t * p_buf, uint32_t len)
{
    if (len == 0 || addr < EXTFLASH_USER_START || addr + len > EXTFLASH_USER_END
        || (addr % EXTFLASH_PAGE) + len > EXTFLASH_PAGE)
    {
        return false;
    }
    if (!wait_ready())
    {
        return false;
    }
    write_enable();
    select();
    xfer(CMD_PAGE_PROGRAM);
    send_addr(addr);
    while (len--)
    {
        xfer(*p_buf++);
    }
    deselect();
    return wait_ready();
}

/* Снять защиту записи только в ОЗУ флеш: 50h (Write Enable for Volatile SR),
 * затем 01h SR1 SR2. Энергонезависимые BP/CMP не меняются: после отключения
 * питания флеш снова защищена, как настроила заводская прошивка. QE (SR2 бит 1)
 * сохраняем. Если регистры заблокированы (SRP1), флеш команду проигнорирует. */
void extflash_unprotect_volatile(uint8_t sr_after[2])
{
    uint8_t sr2 = read_reg(CMD_RDSR2);

    select();
    xfer(0x50);
    deselect();
    select();
    xfer(0x01);
    xfer(0x00);                 /* SR1: BP/TB/SEC = 0, SRP0 = 0 */
    xfer((uint8_t)(sr2 & 0x02)); /* SR2: CMP = 0, остальное 0, QE как было */
    deselect();
    wait_ready();
    extflash_status(sr_after);
}
