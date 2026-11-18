#!/usr/bin/env python3
"""No.1 F4: прошивка по BLE с компьютера (legacy DFU, загрузчик nRF5 SDK 10).

Работает со встроенным Bluetooth Windows/macOS/Linux через bleak, донгл Nordic
не нужен. Понимает DFU-пакеты из f4gfx.py pack / make (только application).

  pip install bleak
  python f4dfu.py app/build/f4app_dfu.zip          # приложение -> DFU -> прошивка
  python f4dfu.py fw.zip --addr C2:13:ED:04:A3:C9  # MAC приложения
  python f4dfu.py fw.zip --no-enter                # браслет уже в DfuTarg

Порядок (ble_dfu.c / dfu_transport_ble.c из SDK 10):
  CP 01 04            -> PKT sizes(sd, bl, app)      -> 10 01 01
  CP 02 00 -> PKT .dat -> CP 02 01                    -> 10 02 01
  CP 08 N             (квитанция каждые N пакетов)
  CP 03 -> PKT данные по 20 байт, 11 <байт> каждые N -> 10 03 01
  CP 04                                               -> 10 04 01
  CP 05               активация и перезагрузка
"""
import argparse
import asyncio
import json
import struct
import sys
import time
import zipfile

from bleak import BleakClient, BleakScanner

APP_SERVICE = "c3e6fea0-e966-1000-8000-be99c223df6a"
APP_CTRL = "c3e6fea1-e966-1000-8000-be99c223df6a"
CMD_DFU = 0x42

DFU_SERVICE = "00001530-1212-efde-1523-785feabcd123"
DFU_CP = "00001531-1212-efde-1523-785feabcd123"
DFU_PKT = "00001532-1212-efde-1523-785feabcd123"

OP_START, OP_INIT, OP_RECEIVE, OP_VALIDATE, OP_ACTIVATE = 1, 2, 3, 4, 5
OP_PRN_REQ, OP_RESPONSE, OP_PRN = 8, 0x10, 0x11
IMAGE_APP = 0x04
INIT_RX, INIT_COMPLETE = 0, 1
RESP_TEXT = {1: "успех", 2: "неверное состояние", 3: "не поддерживается",
             4: "превышен размер", 5: "ошибка CRC", 6: "операция не удалась"}

PACKET = 20


def log(msg):
    print(f"{time.strftime('%H:%M:%S')}  {msg}", flush=True)


def load_zip(path):
    with zipfile.ZipFile(path) as z:
        app = json.loads(z.read("manifest.json"))["manifest"].get("application")
        if app is None:
            sys.exit("В пакете нет application — SoftDevice/загрузчик не поддерживаются")
        image = z.read(app["bin_file"])
        init = z.read(app["dat_file"])
    if len(image) % 4:
        sys.exit(f"Размер образа {len(image)} не кратен 4 — загрузчик его не примет")
    return image, init


def is_dfu(dev, ad):
    name = (dev.name or ad.local_name or "").lower()
    return DFU_SERVICE in [u.lower() for u in ad.service_uuids] or name.startswith("dfutarg")


async def find(predicate, timeout):
    return await BleakScanner.find_device_by_filter(predicate, timeout=timeout)


async def enter_dfu(addr):
    def is_app(d, ad):
        if addr:
            return d.address.upper() == addr.upper()
        return APP_SERVICE in [u.lower() for u in ad.service_uuids]

    log("ищу браслет (приложение или DfuTarg)...")
    found = {}

    def any_target(d, ad):
        if is_dfu(d, ad):
            found["dfu"] = d
            return True
        if is_app(d, ad):
            found["app"] = d
            return True
        return False

    if await find(any_target, 20) is None:
        sys.exit("Браслет не найден ни как приложение, ни как DfuTarg")
    if "dfu" in found:
        log(f"уже в DFU: {found['dfu'].name} [{found['dfu'].address}]")
        return found["dfu"]

    app = found["app"]
    log(f"приложение: {app.name} [{app.address}], отправляю 0x{CMD_DFU:02X}")
    async with BleakClient(app) as client:
        try:
            await client.write_gatt_char(APP_CTRL, bytes([CMD_DFU]), response=True)
        except Exception as e:  # обрыв из-за перехода в загрузчик — ожидаемо
            log(f"соединение закрыто: {e}")
    dfu = await find(is_dfu, 20)
    if dfu is None:
        sys.exit("DfuTarg не появился. Браслет не перешёл в загрузчик")
    log(f"загрузчик: {dfu.name} [{dfu.address}]")
    return dfu


class Dfu:
    def __init__(self, client):
        self.client = client
        self.responses = asyncio.Queue()
        self.received = 0
        self.prn_event = asyncio.Event()

    def on_notify(self, _, data):
        data = bytes(data)
        if data[0] == OP_RESPONSE:
            self.responses.put_nowait((data[1], data[2]))
        elif data[0] == OP_PRN:
            self.received = struct.unpack_from("<I", data, 1)[0]
            self.prn_event.set()

    async def cp(self, *payload):
        await self.client.write_gatt_char(DFU_CP, bytes(payload), response=True)

    async def pkt(self, data):
        await self.client.write_gatt_char(DFU_PKT, data, response=False)

    async def expect(self, op, timeout):
        try:
            got_op, status = await asyncio.wait_for(self.responses.get(), timeout)
        except asyncio.TimeoutError:
            raise RuntimeError(f"нет ответа на операцию {op} за {timeout} с")
        if got_op != op or status != 1:
            raise RuntimeError(f"операция {got_op}: {RESP_TEXT.get(status, status)}")

    async def run(self, image, init, prn):
        await self.client.start_notify(DFU_CP, self.on_notify)

        log(f"старт: приложение {len(image)} байт (стирание флеша)")
        await self.cp(OP_START, IMAGE_APP)
        await self.pkt(struct.pack("<III", 0, 0, len(image)))
        await self.expect(OP_START, 30)

        log(f"init packet {init.hex(' ')}")
        await self.cp(OP_INIT, INIT_RX)
        await self.pkt(init)
        await self.cp(OP_INIT, INIT_COMPLETE)
        await self.expect(OP_INIT, 10)

        await self.cp(OP_PRN_REQ, *struct.pack("<H", prn))
        await self.cp(OP_RECEIVE)

        t0 = time.time()
        packets = [image[i:i + PACKET] for i in range(0, len(image), PACKET)]
        for n, chunk in enumerate(packets, 1):
            await self.pkt(chunk)
            if n % prn == 0 and n != len(packets):
                # Ждём квитанцию: иначе переполним буферы загрузчика.
                self.prn_event.clear()
                try:
                    await asyncio.wait_for(self.prn_event.wait(), 10)
                except asyncio.TimeoutError:
                    raise RuntimeError(f"нет квитанции после {n * PACKET} байт")
                sent = min(n * PACKET, len(image))
                if self.received != sent:
                    raise RuntimeError(f"загрузчик принял {self.received}, отправлено {sent}")
                rate = sent / max(time.time() - t0, 1e-3)
                print(f"\r  {sent}/{len(image)} байт, {rate / 1024:.1f} КБ/с ", end="", flush=True)
        await self.expect(OP_RECEIVE, 30)
        print(f"\r  {len(image)}/{len(image)} байт за {time.time() - t0:.1f} с          ")

        log("проверка CRC")
        await self.cp(OP_VALIDATE)
        await self.expect(OP_VALIDATE, 10)

        log("активация и перезагрузка")
        try:
            await self.cp(OP_ACTIVATE)
        except Exception:  # загрузчик рвёт связь при перезагрузке
            pass


async def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("zip", help="DFU-пакет (manifest.json + .bin + .dat)")
    p.add_argument("--addr", help="MAC приложения, если не искать по сервису FEA0")
    p.add_argument("--no-enter", action="store_true", help="браслет уже в DfuTarg")
    p.add_argument("--prn", type=int, default=8, help="квитанция каждые N пакетов (8)")
    a = p.parse_args()

    image, init = load_zip(a.zip)
    if a.no_enter:
        dfu = await find(is_dfu, 20)
        if dfu is None:
            sys.exit("DfuTarg не найден")
    else:
        dfu = await enter_dfu(a.addr)

    async with BleakClient(dfu, timeout=20) as client:
        if client.services.get_service(DFU_SERVICE) is None:
            sys.exit("У устройства нет сервиса DFU 0x1530")
        await Dfu(client).run(image, init, a.prn)

    log("готово, жду приложение в эфире...")
    app = await find(lambda d, ad: not is_dfu(d, ad) and (
        (a.addr and d.address.upper() == a.addr.upper())
        or APP_SERVICE in [u.lower() for u in ad.service_uuids]), 30)
    if app:
        log(f"приложение запущено: {app.name} [{app.address}]")
    else:
        log("приложение с сервисом FEA0 не появилось за 30 с (для чужой прошивки это нормально)")


if __name__ == "__main__":
    asyncio.run(main())
