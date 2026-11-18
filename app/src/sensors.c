/* Кнопки, пульсометр, батарея — по заводской прошивке.
 *
 *   кнопки   P0.10 (pull-up), P0.11 (без подтяжки), P0.18, P0.19 (pull-up),
 *            активный уровень 0 — таблица app_button 0x2D8A0/0x2D8B0;
 *   P0.28/29 входы без подтяжки, назначение неизвестно (0x1993C);
 *   пульс    P0.07 — включение (выход, 0 по умолчанию; 0x1B5E4, 0x1B5C4),
 *            P0.01 = AIN2 — сигнал PPG (0x1B9AC, разбор в 0x1B618),
 *            P0.03 = AIN4 — второй канал, вероятно «на руке» (0x1B588);
 *   батарея  P0.02 = AIN3, мВ = adc * 3600000 / 510976 (0x1B3A0);
 *   мотор    P0.00, активный 1 (вкл 0x1C628, выкл 0x199A8, инициализация 0x1C4F0);
 *   P0.12/14 выходы, при старте 0 и 1 (0x195C8), назначение неизвестно.
 * P0.17 и P0.25 — CS устройств на SPI0 (P0.25 — флеш, команда 03), их не трогаем.
 * АЦП как в заводской (конфиг 0x2D9A8): 10 бит, вход 1/3, опора 1.2 В.
 */
#include "sensors.h"

#include "nrf.h"
#include "nrf_delay.h"

#define PIN_MOTOR   0
#define PIN_HR_EN   7
#define PIN_HR      1
#define PIN_HR_AUX  3

#define AIN_HR      ADC_CONFIG_PSEL_AnalogInput2
#define AIN_BAT     ADC_CONFIG_PSEL_AnalogInput3
#define AIN_HR_AUX  ADC_CONFIG_PSEL_AnalogInput4

#define ADC_TIMEOUT 2000

/* Плавный пуск мотора: ШИМ 1 кГц, заполнение от 1/RAMP_STEPS до 100 %
 * за RAMP_STEPS * RAMP_PERIODS мс. Резкое включение при питании от внешнего
 * источника сбрасывало чип (RESETREAS = 0). */
/* 1 — разгон мотора за 100 мс (блокирует): для питания от слабого внешнего
 * источника, где бросок тока сбрасывал F4. На батарее цветной версии не нужен. */
#ifndef MOTOR_SOFT_START
#define MOTOR_SOFT_START    0
#endif
#define MOTOR_PWM_US        1000
#define MOTOR_RAMP_STEPS    20
#define MOTOR_RAMP_PERIODS  5

static const struct { uint8_t pin; uint8_t pull; } m_inputs[] = {
    { 10, GPIO_PIN_CNF_PULL_Pullup },
    { 11, GPIO_PIN_CNF_PULL_Disabled },
    { 18, GPIO_PIN_CNF_PULL_Pullup },
    { 19, GPIO_PIN_CNF_PULL_Pullup },
    { 28, GPIO_PIN_CNF_PULL_Disabled },
    { 29, GPIO_PIN_CNF_PULL_Disabled },
};

static bool m_hr_on;


static void pin_input(uint32_t pin, uint32_t pull)
{
    NRF_GPIO->PIN_CNF[pin] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos)
                           | (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos)
                           | (pull << GPIO_PIN_CNF_PULL_Pos);
}

static void pin_analog(uint32_t pin)
{
    NRF_GPIO->PIN_CNF[pin] = (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos)
                           | (GPIO_PIN_CNF_INPUT_Disconnect << GPIO_PIN_CNF_INPUT_Pos);
}

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

static uint16_t adc_read(uint32_t psel)
{
    uint32_t n = 0;

    NRF_ADC->CONFIG = (ADC_CONFIG_RES_10bit << ADC_CONFIG_RES_Pos)
                    | (ADC_CONFIG_INPSEL_AnalogInputOneThirdPrescaling << ADC_CONFIG_INPSEL_Pos)
                    | (ADC_CONFIG_REFSEL_VBG << ADC_CONFIG_REFSEL_Pos)
                    | (psel << ADC_CONFIG_PSEL_Pos);
    NRF_ADC->ENABLE = ADC_ENABLE_ENABLE_Enabled;
    NRF_ADC->EVENTS_END = 0;
    NRF_ADC->TASKS_START = 1;
    while (NRF_ADC->EVENTS_END == 0 && ++n < ADC_TIMEOUT)
    {
    }
    NRF_ADC->EVENTS_END = 0;
    NRF_ADC->ENABLE = ADC_ENABLE_ENABLE_Disabled;
    return (uint16_t)NRF_ADC->RESULT;
}


void sensors_init(void)
{
    for (uint32_t i = 0; i < sizeof(m_inputs) / sizeof(m_inputs[0]); i++)
    {
        pin_input(m_inputs[i].pin, m_inputs[i].pull);
    }

    pin_output(PIN_MOTOR, false);
    pin_output(PIN_HR_EN, false);
    pin_output(12, false);
    pin_output(14, true);
    pin_analog(PIN_HR);
    pin_analog(PIN_HR_AUX);
}

void sensors_deinit(void)
{
    NRF_ADC->ENABLE = ADC_ENABLE_ENABLE_Disabled;
    sensors_hr_enable(false);
    for (uint32_t i = 0; i < sizeof(m_inputs) / sizeof(m_inputs[0]); i++)
    {
        pin_analog(m_inputs[i].pin);
    }
    for (uint32_t pin = 0; pin < 32; pin++)
    {
        if (pin != PIN_MOTOR && (SENSORS_OUT_MASK & (1u << pin)))
        {
            NRF_GPIO->OUTCLR = 1u << pin;
            pin_analog(pin);
        }
    }
    /* Мотор держим выключенным, а не в воздухе: загрузчик этот пин не трогает. */
    pin_output(PIN_MOTOR, false);
}

uint32_t sensors_inputs(void)
{
    return NRF_GPIO->IN & (SENSORS_BTN_MASK | SENSORS_AUX_MASK);
}

void sensors_hr_enable(bool on)
{
    m_hr_on = on;
    if (on)
    {
        NRF_GPIO->OUTSET = 1u << PIN_HR_EN;
    }
    else
    {
        NRF_GPIO->OUTCLR = 1u << PIN_HR_EN;
    }
}

bool sensors_hr_enabled(void)
{
    return m_hr_on;
}

uint16_t sensors_hr_raw(void)
{
    return adc_read(AIN_HR);
}

uint16_t sensors_hr_aux_raw(void)
{
    return adc_read(AIN_HR_AUX);
}

uint16_t sensors_battery_mv(void)
{
    return (uint16_t)(adc_read(AIN_BAT) * 3600000u / 510976u);
}

void sensors_motor(bool on)
{
    if (!on)
    {
        NRF_GPIO->OUTCLR = 1u << PIN_MOTOR;
        return;
    }
#if !MOTOR_SOFT_START
    /* Цветная версия на батарее: сразу полный ход (пользователь попросил резкий
     * отклик). Плавный пуск нужен был F4 на внешнем источнике, см. MOTOR_SOFT_START. */
    NRF_GPIO->OUTSET = 1u << PIN_MOTOR;
    return;
#endif
    for (uint32_t step = 1; step <= MOTOR_RAMP_STEPS; step++)
    {
        uint32_t on_us = MOTOR_PWM_US * step / MOTOR_RAMP_STEPS;

        for (uint32_t i = 0; i < MOTOR_RAMP_PERIODS; i++)
        {
            NRF_GPIO->OUTSET = 1u << PIN_MOTOR;
            nrf_delay_us(on_us);
            if (on_us < MOTOR_PWM_US)
            {
                NRF_GPIO->OUTCLR = 1u << PIN_MOTOR;
                nrf_delay_us(MOTOR_PWM_US - on_us);
            }
        }
    }
    NRF_GPIO->OUTSET = 1u << PIN_MOTOR;
}

bool sensors_output(uint32_t pin, bool high)
{
    if (pin >= 32 || !(SENSORS_OUT_MASK & (1u << pin)))
    {
        return false;
    }
    if (pin == PIN_HR_EN)
    {
        sensors_hr_enable(high);
        return true;
    }
    pin_output(pin, high);
    return true;
}
