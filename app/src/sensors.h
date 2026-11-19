/* Кнопки, пульсометр, батарея — пины и АЦП по заводской прошивке. */
#ifndef SENSORS_H_INCLUDED
#define SENSORS_H_INCLUDED

#include <stdbool.h>
#include <stdint.h>

/* Входы, за которыми следим (маска для NRF_GPIO->IN). */
#define SENSORS_BTN_MASK  ((1u << 10) | (1u << 11) | (1u << 18) | (1u << 19))
#define SENSORS_AUX_MASK  ((1u << 28) | (1u << 29))

/* Выходы, которые разрешено дёргать командой: пульсометр, P0.12/14 (назначение неизвестно).
 * Мотор (P0.00) — только через sensors_motor(): резкое включение сбрасывает чип по питанию. */
#define SENSORS_OUT_MASK  ((1u << 7) | (1u << 12) | (1u << 14))

/* Что будит из сна: касание P0.11 и начало зарядки P0.29 (оба — уровнем 0). */
#define SENSORS_WAKE_MASK ((1u << 11) | (1u << 29))

void     sensors_init(void);
/* Перед сном: мотор, пульсометр и АЦП выключены, лишние входы отключены от
 * буфера, на P0.11 и P0.29 — SENSE = Low (сигнал DETECT). */
void     sensors_sleep(void);
/* Идёт зарядка (P0.29 = 0, открытый сток микросхемы заряда). */
bool     sensors_charging(void);
/* Пины и АЦП в состояние после сброса — перед переходом в загрузчик. */
void     sensors_deinit(void);
uint32_t sensors_inputs(void);
void     sensors_hr_enable(bool on);
bool     sensors_hr_enabled(void);
uint16_t sensors_hr_raw(void);       /* AIN2 (P0.01), 0..1023 */
uint16_t sensors_hr_aux_raw(void);   /* AIN4 (P0.03), 0..1023 */
uint16_t sensors_battery_mv(void);   /* AIN3 (P0.02), делитель 1/2 */
/* Вибромотор. Включение — с плавным пуском (программный ШИМ ~100 мс, блокирует). */
void     sensors_motor(bool on);
/* Только пины из SENSORS_OUT_MASK. false — пин не разрешён. */
bool     sensors_output(uint32_t pin, bool high);

#endif
