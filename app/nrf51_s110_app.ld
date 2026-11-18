/* No.1 F4: приложение под S110 v8.0.0 на nRF51422 xxAC (256 КБ flash, 32 КБ RAM).
 * FLASH: от 0x18000 до предполагаемого начала загрузчика 0x3C000.
 * RAM:   первые 8 КБ заняты SoftDevice.
 */
SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x18000, LENGTH = 0x24000
  RAM (rwx) :  ORIGIN = 0x20002000, LENGTH = 0x6000
}

INCLUDE "nrf5x_common.ld"
