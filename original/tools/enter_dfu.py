#!/usr/bin/env python3
"""No.1 F4: перевод браслета в режим DFU (загрузчик Nordic, "DfuTarg").

Пишет один байт 0x42 в характеристику FEA1 сервиса FEA0 производителя.
После этого браслет перезагружается в загрузчик; прошивайте ZIP через
nRF Connect / nRF Toolbox (DFU -> Distribution packet).

Запускать в Python под Windows (из WSL Bluetooth недоступен):
  pip install bleak
  python enter_dfu.py                 # поиск по рекламируемому сервису
  python enter_dfu.py AA:BB:CC:DD:EE:FF   # по MAC-адресу
"""
import asyncio
import sys

from bleak import BleakClient, BleakScanner

SERVICE = "c3e6fea0-e966-1000-8000-be99c223df6a"
CHAR = "c3e6fea1-e966-1000-8000-be99c223df6a"
CMD_DFU = b"\x42"


async def find_device(timeout=15):
    return await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE in [u.lower() for u in ad.service_uuids],
        timeout=timeout)


async def main():
    if len(sys.argv) > 1:
        target = sys.argv[1]
    else:
        print("Поиск браслета...")
        target = await find_device()
        if target is None:
            sys.exit("Не найден по сервису FEA0. Укажите MAC: "
                     "python enter_dfu.py AA:BB:CC:DD:EE:FF")
        print(f"Найден: {target.name} [{target.address}]")

    async with BleakClient(target) as client:
        char = client.services.get_characteristic(CHAR)
        if char is None:
            sys.exit("Характеристика FEA1 не найдена — это точно No.1 F4?")
        with_response = "write" in char.properties
        print(f"Отправка 0x42 (свойства: {', '.join(char.properties)})")
        try:
            await client.write_gatt_char(CHAR, CMD_DFU, response=with_response)
        except Exception as e:  # обрыв связи из-за перезагрузки — ожидаемо
            print(f"Соединение закрыто: {e}")

    print("Готово. Пересканируйте устройства и подключитесь к DfuTarg.")


if __name__ == "__main__":
    asyncio.run(main())
