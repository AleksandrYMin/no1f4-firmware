#!/usr/bin/env python3
"""No.1 F4: поиск P0.20 (кнопка DFU загрузчика) на плате.

Опрашивает GPIO->IN через команду peek прошивки app 0.2.x и печатает каждое
изменение. Пищит, когда P0.20 уходит в 0 или в эфире появляется DfuTarg.
При обрыве связи сам ищет браслет и переподключается.

P0.20 на часах ни к чему не подключён, но загрузчик включает на нём
подтяжку к питанию: замыкание вывода на землю даёт 0.

  pip install bleak
  python watch_p020.py                 # только смотреть
  python watch_p020.py --dfu           # при P0.20 = 0 сразу отправить 0x42
  python watch_p020.py --addr AA:BB:CC:DD:EE:FF

С --dfu: держите проводок на P0.20, пока не появится DfuTarg. Загрузчик
проверяет пин при второй перезагрузке (первая уходит на неудачный 0xB1).
Ctrl+C — выход.
"""
import argparse
import asyncio
import struct
import time

from bleak import BleakClient, BleakScanner

try:
    import winsound
except ImportError:  # не Windows
    winsound = None

SERVICE = "c3e6fea0-e966-1000-8000-be99c223df6a"
CTRL = "c3e6fea1-e966-1000-8000-be99c223df6a"
RESP = "c3e6fea2-e966-1000-8000-be99c223df6a"
DFU_SERVICE = "00001530-1212-efde-1523-785feabcd123"
GPIO_IN = 0x50000510
PIN = 20

T0 = time.time()


def log(msg):
    print(f"{time.strftime('%H:%M:%S')} {time.time() - T0:7.1f}s  {msg}", flush=True)


def beep(freq, ms):
    if winsound:
        winsound.Beep(freq, ms)
    else:
        print("\a", end="", flush=True)


async def scan(addr, seconds):
    """BLEDevice браслета (наш сервис или заданный адрес) или None. DfuTarg — в лог."""
    found = {}

    def cb(d, ad):
        name = d.name or ad.local_name or ""
        uuids = [u.lower() for u in ad.service_uuids]
        if DFU_SERVICE in uuids or "dfu" in name.lower():
            if d.address not in found:
                log(f"!!! В ЭФИРЕ DFU: {d.address} {name} — прошивайте ZIP через nRF Connect")
                beep(2000, 600)
            found[d.address] = d
        elif (addr and d.address.upper() == addr.upper()) or (not addr and SERVICE in uuids):
            found["app"] = d

    scanner = BleakScanner(cb)
    await scanner.start()
    t = time.time()
    while time.time() - t < seconds and "app" not in found:
        await asyncio.sleep(0.3)
    await scanner.stop()
    return found.get("app")


async def watch(dev, auto_dfu):
    prev = None
    async with BleakClient(dev, timeout=20) as client:
        queue = asyncio.Queue()
        await client.start_notify(RESP, lambda _, d: queue.put_nowait(bytes(d)))
        log(f"подключено к {dev.name} [{dev.address}]")
        while True:
            await client.write_gatt_char(CTRL, b"\x10" + struct.pack("<IB", GPIO_IN, 4), response=True)
            resp = await asyncio.wait_for(queue.get(), 5)
            value = struct.unpack_from("<I", resp, 5)[0]
            if value != prev:
                changed = "" if prev is None else "  изменились: " + ", ".join(
                    f"P0.{p:02d}={value >> p & 1}" for p in range(32) if (value ^ prev) >> p & 1)
                log(f"IN={value:#010x}  P0.20={value >> PIN & 1}{changed}")
                if not value >> PIN & 1:
                    log(">>> P0.20 = 0 — ЭТО ОН")
                    beep(1500, 300)
                    if auto_dfu:
                        log(">>> отправляю 0x42, ДЕРЖИТЕ проводок до появления DfuTarg")
                        try:
                            await client.write_gatt_char(CTRL, b"\x42", response=True)
                        except Exception as e:  # обрыв из-за перезагрузки — ожидаемо
                            log(f"соединение закрыто: {e}")
                        return
                prev = value
            await asyncio.sleep(0.2)


async def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--addr", help="MAC-адрес вместо поиска по сервису")
    p.add_argument("--dfu", action="store_true", help="при P0.20 = 0 отправить 0x42")
    a = p.parse_args()

    log("старт. Ctrl+C — выход")
    while True:
        dev = await scan(a.addr, 10)
        if dev is None:
            log("браслета нет в эфире (питание? передёрните)")
            continue
        try:
            await watch(dev, a.dfu)
        except Exception as e:
            log(f"связь потеряна: {type(e).__name__} {e}")
            beep(600, 150)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
