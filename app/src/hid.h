/* BLE HID (HID over GATT): мультимедийные клавиши — спуск камеры телефона. */
#ifndef HID_H_INCLUDED
#define HID_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

#include "ble.h"

/* Сервисы HID (0x1812), Battery (0x180F), Device Information (0x180A).
 * Вызывать после sd_ble_enable(). Возвращает NRF_SUCCESS или код ошибки SoftDevice. */
uint32_t hid_init(void);
/* Все события BLE: соединение, безопасность, CCCD, TX_COMPLETE. */
void hid_on_ble_evt(const ble_evt_t * p_evt);
/* Нажать и отпустить Volume Up. false — не отправлено (нет телефона, нет шифрования
 * или он не подписался на отчёты). */
bool hid_volume_up_click(void);
/* Уровень батареи для Battery Service, 0..100. */
void hid_battery_set(uint8_t percent);
/* Телефон подключён, связь зашифрована и подписка на отчёты включена. */
bool hid_ready(void);

#endif
