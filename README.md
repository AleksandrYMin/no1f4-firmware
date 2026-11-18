# No.1 F4 — собственная прошивка / custom firmware

[Русский](#русский) · [English](#english)

---

## Русский

Собственная прошивка и заметки по реверс-инжинирингу фитнес-браслета **No.1 F4** и его версии с цветным экраном. Прошивка ставится по воздуху через заводской загрузчик Nordic, без программатора и вскрытия корпуса.

Спецификация браслета от производителя: [original/12-2.jpg](original/12-2.jpg).

### Железо

| | No.1 F4 | Цветная версия («Smart Band») |
|---|---|---|
| Процессор | nRF51422 (Cortex-M0, 256 КБ флеш, 32 КБ ОЗУ), SoftDevice S110 v8.0.0 | то же |
| Экран | OLED 0,96″ 64×128, SH1107 | TFT 0,96″ 80×160, ST7735S |
| Внешняя флеш | 2 МБ SPI (Boya BY25Q16) | 2 МБ SPI (Zbit, `5E 40 15`) |
| Загрузчик | legacy DFU из nRF5 SDK 10 (`DfuTarg52`) | то же, другая сборка (`DfuTarg00645`) |
| Кнопка / мотор | сенсорная P0.11 / P0.00 | то же |

Подробности, включая пины, карту внешней флеш и разбор заводской прошивки, — в [RE_NOTES.md](RE_NOTES.md).

### Что умеет прошивка (`app/`, версия 0.10.0)

- Драйвер цветного TFT (ST7735S) и монохромного OLED (SH1107, сборка с `DISPLAY_SH1107=1`).
- Гашение экрана через 10 с, пробуждение касанием.
- По касанию: вибрация и картинка или GIF-анимация, загруженная с компьютера во внешнюю флеш.
- **Bluetooth-спуск камеры телефона:** браслет становится BLE-клавиатурой (HID) и по касанию посылает «громкость +». Проверено на Android.
- Обновление по воздуху через заводской загрузчик сохраняется: команда `0x42` уводит в DFU, как в заводской прошивке.
- Служебный протокол по BLE: чтение памяти, дампы, управление пинами, сырые команды дисплею, чтение и запись внешней флеш.

### Сборка

Нужны (в репозиторий не входят, кладутся в `toolchain/`):
- Arm GNU Toolchain 13.3.rel1 (`arm-none-eabi`) → `toolchain/gcc-arm/`;
- nRF5 SDK 10.0.0 → `toolchain/nrf5_sdk_10/`. Используется зеркало `github.com/yedapeng/NRF51-SDK10.0`; `dfu_types.h` из него не использовать (см. RE_NOTES §8a).

```bash
cd app
make clean && make        # -> build/f4app_dfu.zip
```

После правки заголовков всегда делайте `make clean`: в Makefile нет зависимостей от `.h`.

### Прошивка и утилиты

Python 3 с пакетами `bleak` и `Pillow`, встроенный Bluetooth компьютера (Windows).

```bash
pip install bleak pillow
python app/tools/f4dfu.py app/build/f4app_dfu.zip --addr AA:BB:CC:DD:EE:FF   # прошить по BLE
python app/tools/f4ctl.py --addr AA:BB:CC:DD:EE:FF ping                      # версия
python app/tools/f4ctl.py --addr AA:BB:CC:DD:EE:FF image photo.jpg           # картинка по касанию
python app/tools/f4ctl.py --addr AA:BB:CC:DD:EE:FF anim clip.gif             # анимация по касанию
```

Полный список команд `f4ctl.py` — в RE_NOTES §8a. Пока к браслету подключён телефон (как HID), компьютер подключиться не может: выключите Bluetooth на телефоне.

### Предупреждения

- **Заводская прошивка будет стёрта** — копии в DFU-пакете производителя для цветной версии нет. Для F4 пакет лежит в [original/](original/).
- SWD закрыт (UICR PALL): если сломать путь в DFU, восстановление — только вскрытием (P0.20 на землю при сбросе) или полным стиранием по SWD.
- Всё проверено на двух экземплярах. На других ревизиях платы пины и периферия могут отличаться.

### Структура

| Путь | Что там |
|---|---|
| `app/` | исходники прошивки, Makefile, утилиты `f4dfu.py`, `f4ctl.py` |
| `RE_NOTES.md` | заметки по реверсу и железу обеих версий |
| `backup/` | дампы загрузчиков, UICR/FICR и внешней флеш цветной версии |
| `original/` | заводской DFU-пакет F4, графика из него, `f4gfx.py`, `enter_dfu.py` |

---

## English

Custom firmware and reverse-engineering notes for the **No.1 F4** fitness band and its colour-screen variant. The firmware is installed over the air through the stock Nordic bootloader — no programmer, no need to open the case.

Manufacturer's spec sheet: [original/12-2.jpg](original/12-2.jpg).

### Hardware

| | No.1 F4 | Colour variant ("Smart Band") |
|---|---|---|
| MCU | nRF51422 (Cortex-M0, 256 KB flash, 32 KB RAM), SoftDevice S110 v8.0.0 | same |
| Display | 0.96″ OLED 64×128, SH1107 | 0.96″ TFT 80×160, ST7735S |
| External flash | 2 MB SPI (Boya BY25Q16) | 2 MB SPI (Zbit, `5E 40 15`) |
| Bootloader | nRF5 SDK 10 legacy DFU (`DfuTarg52`) | same, different build (`DfuTarg00645`) |
| Button / motor | touch P0.11 / P0.00 | same |

Details — pinout, external flash map, analysis of the stock firmware — are in [RE_NOTES.md](RE_NOTES.md) (in Russian).

### Firmware features (`app/`, version 0.10.0)

- Colour TFT driver (ST7735S) and monochrome OLED driver (SH1107, build with `DISPLAY_SH1107=1`).
- Screen turns off after 10 s, wakes on touch.
- On touch: vibration and a picture or GIF animation uploaded from a PC to the external flash.
- **Bluetooth camera shutter:** the band acts as a BLE keyboard (HID) and sends "Volume Up" on touch. Tested on Android.
- Over-the-air updates through the stock bootloader keep working: command `0x42` enters DFU, just like the stock firmware.
- Service protocol over BLE: memory reads, dumps, pin control, raw display commands, external flash read and write.

### Building

Required (not included in the repository, placed under `toolchain/`):
- Arm GNU Toolchain 13.3.rel1 (`arm-none-eabi`) → `toolchain/gcc-arm/`;
- nRF5 SDK 10.0.0 → `toolchain/nrf5_sdk_10/`. The mirror `github.com/yedapeng/NRF51-SDK10.0` is used; do not use its `dfu_types.h` (see RE_NOTES §8a).

```bash
cd app
make clean && make        # -> build/f4app_dfu.zip
```

Always run `make clean` after editing headers: the Makefile has no `.h` dependencies.

### Flashing and tools

Python 3 with `bleak` and `Pillow`, the PC's built-in Bluetooth (Windows).

```bash
pip install bleak pillow
python app/tools/f4dfu.py app/build/f4app_dfu.zip --addr AA:BB:CC:DD:EE:FF   # flash over BLE
python app/tools/f4ctl.py --addr AA:BB:CC:DD:EE:FF ping                      # version
python app/tools/f4ctl.py --addr AA:BB:CC:DD:EE:FF image photo.jpg           # picture on touch
python app/tools/f4ctl.py --addr AA:BB:CC:DD:EE:FF anim clip.gif             # animation on touch
```

The full list of `f4ctl.py` commands is in RE_NOTES §8a. While a phone is connected to the band (as HID), the PC cannot connect: turn Bluetooth off on the phone.

### Warnings

- **The stock firmware will be erased.** There is no vendor DFU package for the colour variant; the F4 package is in [original/](original/).
- SWD is locked (UICR PALL): if the DFU path breaks, recovery requires opening the case (P0.20 to ground during reset) or a full SWD erase.
- Everything was tested on two units. Other board revisions may use different pins and peripherals.

### Layout

| Path | Contents |
|---|---|
| `app/` | firmware sources, Makefile, `f4dfu.py` and `f4ctl.py` tools |
| `RE_NOTES.md` | reverse-engineering and hardware notes for both variants |
| `backup/` | bootloader, UICR/FICR and colour-variant external flash dumps |
| `original/` | stock F4 DFU package, its graphics, `f4gfx.py`, `enter_dfu.py` |
