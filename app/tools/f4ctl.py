#!/usr/bin/env python3
"""No.1 F4: управление собственной прошивкой (app/) по BLE.

Протокол — см. app/src/main.c: команды пишутся в FEA1, ответ приходит
уведомлением из FEA2.

  pip install bleak
  python f4ctl.py ping
  python f4ctl.py peek 0x10001014 4          # UICR.BOOTLOADERADDR
  python f4ctl.py dump 0x3C000 0x4000 bl.bin # снять загрузчик
  python f4ctl.py dfu                        # перезагрузка в DfuTarg
  python f4ctl.py inputs                     # нажатия кнопок (0.3+)
  python f4ctl.py hr 30 --csv ppg.csv        # пульсометр 30 с, отсчёты в CSV (0.3+)
  python f4ctl.py gpio 12 1                  # выход P0.12 в 1 (только 7/12/14)
  python f4ctl.py motor 500                  # вибрация 500 мс (0.4+)
  python f4ctl.py flash-id                   # JEDEC ID внешней флеш (0.4+)
  python f4ctl.py flash-dump 0 0x100000 ext.bin  # снять внешнюю флеш (0.4+)
  python f4ctl.py pin 15 out1                # любой пин: in, up, down, out0, out1 (0.5+)
  python f4ctl.py lcd 29 11 --cs 24 --dc 22  # команда 0x29 (DISPON), параметры следом (0.5+)
  python f4ctl.py lcd-fill F800 --count 57600 --cs 24 --dc 22   # залить красным (0.5+)
  python f4ctl.py lcd-read 04 4 --cs 24 --dc 22 [--3wire]       # RDDID (0.5+)
  python f4ctl.py flash-status               # SR1/SR2 внешней флеш, биты защиты (0.8+)
  python f4ctl.py flash-write 0x1A0000 data.bin   # стереть сектора и записать, с проверкой (0.8+)
  python f4ctl.py image photo.jpg            # картинка по касанию: 80x160 RGB565 на 0x1A0000 (0.8+)
  python f4ctl.py --addr AA:BB:CC:DD:EE:FF ping

С заводской прошивкой работает только dfu (та же команда 0x42).
"""
import argparse
import asyncio
import struct
import sys
import time

from bleak import BleakClient, BleakScanner

SERVICE = "c3e6fea0-e966-1000-8000-be99c223df6a"
CTRL = "c3e6fea1-e966-1000-8000-be99c223df6a"
RESP = "c3e6fea2-e966-1000-8000-be99c223df6a"

CMD_PING, CMD_PEEK, CMD_HR, CMD_GPIO, CMD_DFU = 0x01, 0x10, 0x20, 0x30, 0x42
CMD_MOTOR, CMD_FLASH_ID, CMD_FLASH_READ = 0x31, 0x40, 0x41
CMD_PIN, CMD_LCD_WRITE, CMD_LCD_FILL, CMD_LCD_READ = 0x32, 0x50, 0x51, 0x52
PIN_MODES = {"in": 0, "up": 1, "down": 2, "out0": 3, "out1": 4}
CMD_FLASH_ERASE, CMD_FLASH_WRITE, CMD_FLASH_STATUS, CMD_FLASH_UNPROTECT = 0x45, 0x46, 0x47, 0x48
USER_START, USER_END, SECTOR, PAGE = 0x1A0000, 0x1F0000, 4096, 256
IMAGE_ADDR, SCREEN_W, SCREEN_H = USER_START, 80, 160
WRITE_CHUNK = 16  # 1 + 3 + 16 = 20 байт — максимум для FEA1
EVT_INPUTS, EVT_PPG, EVT_FLASH, RESP_ERROR = 0x21, 0x22, 0x44, 0xEE
PEEK_MAX = 15
WATCHED = (10, 11, 18, 19, 28, 29)
TICK_HZ = 25


async def find(addr):
    if addr:
        return addr
    print("Поиск браслета...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: SERVICE in [u.lower() for u in ad.service_uuids], timeout=15)
    if dev is None:
        sys.exit("Не найден по сервису FEA0. Укажите --addr AA:BB:CC:DD:EE:FF")
    print(f"Найден: {dev.name} [{dev.address}]")
    return dev


class F4:
    def __init__(self, client):
        self.client = client
        self.queue = asyncio.Queue()
        self.events = asyncio.Queue()

    def on_notify(self, _, data):
        data = bytes(data)
        # Уведомления о входах и PPG приходят сами — не путать их с ответами на команды.
        (self.events if data[0] in (EVT_INPUTS, EVT_PPG, EVT_FLASH) else self.queue).put_nowait(data)

    async def start(self):
        await self.client.start_notify(RESP, self.on_notify)

    async def call(self, payload, timeout=5):
        await self.client.write_gatt_char(CTRL, bytes(payload), response=True)
        resp = await asyncio.wait_for(self.queue.get(), timeout)
        if resp[0] == RESP_ERROR:
            raise RuntimeError(f"браслет отклонил команду 0x{resp[1]:02X}")
        return resp

    async def peek(self, addr, n):
        resp = await self.call([CMD_PEEK, *struct.pack("<I", addr), n])
        got = struct.unpack_from("<I", resp, 1)[0]
        if got != addr or len(resp) != 5 + n:
            raise RuntimeError(f"неожиданный ответ {resp.hex()}")
        return resp[5:]


async def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--addr", help="MAC-адрес вместо поиска по сервису")
    sub = p.add_subparsers(dest="cmd", required=True)
    sub.add_parser("ping")
    s = sub.add_parser("peek")
    s.add_argument("address", type=lambda x: int(x, 0))
    s.add_argument("n", type=lambda x: int(x, 0), nargs="?", default=4)
    s = sub.add_parser("dump")
    s.add_argument("address", type=lambda x: int(x, 0))
    s.add_argument("size", type=lambda x: int(x, 0))
    s.add_argument("out")
    sub.add_parser("dfu")
    s = sub.add_parser("inputs")
    s.add_argument("seconds", type=float, nargs="?", default=60)
    s = sub.add_parser("hr")
    s.add_argument("seconds", type=float, nargs="?", default=30)
    s.add_argument("--csv")
    s = sub.add_parser("gpio")
    s.add_argument("pin", type=int)
    s.add_argument("level", type=int, choices=(0, 1))
    s = sub.add_parser("motor")
    s.add_argument("ms", type=int, help="длительность, 0 — выключить")
    sub.add_parser("flash-id")
    s = sub.add_parser("flash-dump")
    s.add_argument("address", type=lambda x: int(x, 0))
    s.add_argument("size", type=lambda x: int(x, 0))
    s.add_argument("out")
    s.add_argument("--resume", action="store_true", help="продолжить по размеру файла")
    s = sub.add_parser("pin")
    s.add_argument("pin", type=int)
    s.add_argument("mode", choices=PIN_MODES)
    for name in ("lcd", "lcd-fill", "lcd-read"):
        s = sub.add_parser(name)
        if name == "lcd":
            s.add_argument("bytes", nargs="+", help="hex: команда, затем параметры")
            s.add_argument("--data", action="store_true", help="все байты — данные (D/C = 1)")
        elif name == "lcd-fill":
            s.add_argument("color", help="RGB565 hex, например F800")
            s.add_argument("--count", type=lambda x: int(x, 0), default=240 * 240)
        else:
            s.add_argument("command", help="hex")
            s.add_argument("n", type=int, nargs="?", default=4)
            s.add_argument("--3wire", dest="three", action="store_true", help="ответ по линии SDA (MOSI)")
        s.add_argument("--cs", type=int, default=24)
        s.add_argument("--dc", type=int, default=22, help="255 — без D/C")
        s.add_argument("--mode", type=int, default=0, choices=range(4), help="режим SPI")
    sub.add_parser("flash-status")
    sub.add_parser("flash-unprotect")
    s = sub.add_parser("flash-erase")
    s.add_argument("address", type=lambda x: int(x, 0))
    s.add_argument("size", type=lambda x: int(x, 0))
    s = sub.add_parser("flash-write")
    s.add_argument("address", type=lambda x: int(x, 0))
    s.add_argument("file")
    s = sub.add_parser("image")
    s.add_argument("file", help="PNG/JPG; вписывается в 80x160 с обрезкой краёв")
    s.add_argument("--fit", action="store_true", help="вписать целиком, с чёрными полями")
    s.add_argument("--preview", help="сохранить, что получилось, в PNG и не прошивать")
    s = sub.add_parser("anim")
    s.add_argument("file", help="GIF; вписывается в 80x160 целиком, с сохранением пропорций")
    s.add_argument("--preview", help="сохранить кадры листом PNG и не прошивать")
    a = p.parse_args()

    if a.cmd == "anim" and a.preview:
        from PIL import Image
        data, frames, (w, h, ms) = anim_bytes(a.file)
        sheet = Image.new("RGB", ((w + 2) * len(frames), h))
        for i, fr in enumerate(frames):
            sheet.paste(fr, (i * (w + 2), 0))
        sheet.save(a.preview)
        free = USER_END - IMAGE_ADDR
        print(f"{len(frames)} кадров {w}x{h}, {ms} мс; {len(data)} байт из {free} свободных -> {a.preview}")
        return

    if a.cmd == "image" and a.preview:
        _, im = image_bytes(a.file, a.fit)
        im.resize((SCREEN_W * 4, SCREEN_H * 4), 0).save(a.preview)
        print(f"Предпросмотр: {a.preview}")
        return

    async with BleakClient(await find(a.addr)) as client:
        if a.cmd == "dfu":
            try:
                await client.write_gatt_char(CTRL, bytes([CMD_DFU]), response=True)
            except Exception as e:  # обрыв связи из-за перезагрузки — ожидаемо
                print(f"Соединение закрыто: {e}")
            print("Готово. Подключитесь к DfuTarg и прошейте ZIP.")
            return

        f4 = F4(client)
        await f4.start()
        if a.cmd == "ping":
            resp = await f4.call([CMD_PING])
            print(f"Прошивка: {resp[1:].decode(errors='replace')}")
        elif a.cmd == "peek":
            if not 1 <= a.n <= PEEK_MAX:
                sys.exit(f"n от 1 до {PEEK_MAX}")
            data = await f4.peek(a.address, a.n)
            print(f"0x{a.address:08X}: {data.hex(' ')}")
        elif a.cmd == "dump":
            out = bytearray()
            while len(out) < a.size:
                n = min(PEEK_MAX, a.size - len(out))
                out += await f4.peek(a.address + len(out), n)
                print(f"\r{len(out)}/{a.size}", end="", flush=True)
            open(a.out, "wb").write(out)
            print(f"\nЗаписано {a.out}")
        elif a.cmd == "gpio":
            await f4.call([CMD_GPIO, a.pin, a.level])
            print(f"P0.{a.pin:02d} = {a.level}")
        elif a.cmd == "motor":
            await f4.call([CMD_MOTOR, *struct.pack("<H", a.ms)])
            print(f"вибрация {a.ms} мс" if a.ms else "вибрация выключена")
        elif a.cmd == "pin":
            resp = await f4.call([CMD_PIN, a.pin, PIN_MODES[a.mode]])
            gin = struct.unpack_from("<I", resp, 3)[0]
            print(f"P0.{a.pin:02d} -> {a.mode}; вход P0.{a.pin:02d} = {gin >> a.pin & 1}; GPIO.IN = {gin:08X}")
        elif a.cmd == "lcd":
            data = bytes.fromhex("".join(a.bytes))
            if not 1 <= len(data) <= 16:
                sys.exit("от 1 до 16 байт")
            flags = a.mode | (0x04 if a.data else 0)
            await f4.call([CMD_LCD_WRITE, a.cs, a.dc, flags, *data])
            print(f"отправлено: {data.hex(' ')}")
        elif a.cmd == "lcd-fill":
            await f4.call([CMD_LCD_FILL, a.cs, a.dc, a.mode, *struct.pack("<H", int(a.color, 16)),
                           *struct.pack("<I", a.count)[:3]], timeout=15)
            print(f"залито {a.count} пикселей цветом {a.color}")
        elif a.cmd == "lcd-read":
            flags = a.mode | (0x08 if a.three else 0)
            resp = await f4.call([CMD_LCD_READ, a.cs, a.dc, flags, int(a.command, 16), a.n])
            raw = resp[2:]
            bits = int.from_bytes(raw, "big")
            print(f"0x{resp[1]:02X} -> {raw.hex(' ')}  (со сдвигом на 1 бит: "
                  f"{(bits << 1 & ((1 << 8 * len(raw)) - 1)).to_bytes(len(raw), 'big').hex(' ')})")
        elif a.cmd == "flash-status":
            resp = await f4.call([CMD_FLASH_STATUS])
            sr1, sr2 = resp[1], resp[2]
            bp = sr1 >> 2 & 0x1F
            print(f"SR1 = {sr1:02X}, SR2 = {sr2:02X}; занята (WIP) = {sr1 & 1}, "
                  f"WEL = {sr1 >> 1 & 1}, BP/TB/SEC = {bp:05b}, SRP0 = {sr1 >> 7}, CMP = {sr2 >> 6 & 1}")
            print("Защита от записи выключена" if bp == 0 else "ВКЛЮЧЕНА защита части массива")
        elif a.cmd == "flash-unprotect":
            await flash_unprotect(f4)
        elif a.cmd == "flash-erase":
            await flash_erase(f4, a.address, a.size)
        elif a.cmd == "flash-write":
            await flash_write(f4, a.address, open(a.file, "rb").read())
        elif a.cmd == "image":
            data, _ = image_bytes(a.file, a.fit)
            await flash_write(f4, IMAGE_ADDR, data)
            print("Готово: коснитесь кнопки на часах")
        elif a.cmd == "anim":
            data, frames, (w, h, ms) = anim_bytes(a.file)
            if IMAGE_ADDR + len(data) > USER_END:
                sys.exit(f"Не помещается: {len(data)} байт, свободно {USER_END - IMAGE_ADDR}")
            print(f"{len(frames)} кадров {w}x{h}, {ms} мс, {len(data)} байт")
            await flash_write(f4, IMAGE_ADDR, data)
            print("Готово: коснитесь кнопки на часах")
        elif a.cmd == "flash-id":
            resp = await f4.call([CMD_FLASH_ID])
            describe_jedec(resp[1:4])
        elif a.cmd == "flash-dump":
            if not await flash_dump(f4, a.address, a.size, a.out, a.resume):
                sys.exit(2)
        elif a.cmd == "inputs":
            await watch_inputs(f4, a.seconds)
        elif a.cmd == "hr":
            await record_hr(f4, a.seconds, a.csv)


JEDEC_MAKERS = {0xEF: "Winbond", 0xC8: "GigaDevice", 0xC2: "Macronix", 0x20: "Micron/XMC",
                0x1C: "EON", 0x85: "Puya", 0x9D: "ISSI", 0x5E: "Zbit", 0x68: "Boya", 0x0B: "XTX"}


def describe_jedec(jid):
    maker, kind, cap = jid
    if jid in (b"\x00\x00\x00", b"\xff\xff\xff"):
        print(f"JEDEC ID {jid.hex(' ')} — флеш не отвечает")
        return None
    size = 1 << cap if 16 <= cap <= 28 else None
    print(f"JEDEC ID {jid.hex(' ')}: {JEDEC_MAKERS.get(maker, 'производитель ?')}, тип 0x{kind:02X}, "
          + (f"объём {size // 1024} КБ ({size * 8 // 1024 // 1024} Мбит)" if size else "объём не распознан"))
    return size


def image_bytes(path, fit=False):
    """Картинка -> заголовок IMG1 w h + RGB565 big-endian построчно (как ждёт прошивка 0.8+)."""
    from PIL import Image, ImageOps
    im = ImageOps.exif_transpose(Image.open(path)).convert("RGB")
    if fit:
        im = ImageOps.pad(im, (SCREEN_W, SCREEN_H), Image.LANCZOS, color=(0, 0, 0))
    else:
        im = ImageOps.fit(im, (SCREEN_W, SCREEN_H), Image.LANCZOS)
    out = bytearray(b"IMG1" + struct.pack("<HH", SCREEN_W, SCREEN_H))
    shown = Image.new("RGB", im.size)
    for y in range(SCREEN_H):
        for x in range(SCREEN_W):
            r, g, b = im.getpixel((x, y))
            v = (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3)
            out += struct.pack(">H", v)
            shown.putpixel((x, y), ((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31))
    return bytes(out), shown


async def flash_unprotect(f4):
    """Временно (до отключения питания флеш) снять защиту; True, если биты BP и CMP сброшены."""
    resp = await f4.call([CMD_FLASH_UNPROTECT])
    sr1, sr2 = resp[1], resp[2]
    ok = (sr1 & 0x7C) == 0  # SR2 (CMP) у этой флеш читается как FF — проверка записью
    print(f"  защита: SR1 = {sr1:02X}, SR2 = {sr2:02X} -> {'снята' if ok else 'НЕ снята'}")
    return ok


async def flash_erase(f4, addr, size):
    """Стереть сектора [addr, addr + size) и проверить, что там одни FF."""
    if addr % SECTOR or size % SECTOR or addr < USER_START or addr + size > USER_END:
        sys.exit(f"Только целые сектора 4 КБ внутри 0x{USER_START:06X}..0x{USER_END:06X}")
    if not await flash_unprotect(f4):
        sys.exit("Защиту записи снять не удалось")
    for sec in range(addr, addr + size, SECTOR):
        await f4.call([CMD_FLASH_ERASE, *struct.pack("<I", sec)[:3]], timeout=10)
        print(f"\r  стёрт сектор 0x{sec:06X}", end="", flush=True)
    print()
    import os
    import tempfile
    tmp = os.path.join(tempfile.gettempdir(), "f4_verify.bin")
    await flash_dump(f4, addr, size, tmp)
    back = open(tmp, "rb").read()
    os.remove(tmp)
    if len(back) != size or back.count(0xFF) != size:
        sys.exit("ПРОВЕРКА НЕ ПРОШЛА: после стирания не все байты FF")
    print(f"Стёрто и проверено: 0x{addr:06X}..0x{addr + size - 1:06X}")


async def flash_write(f4, addr, data):
    """Стереть затронутые сектора, записать по 16 байт (не через границу страницы), прочитать и сравнить."""
    end = addr + len(data)
    if addr < USER_START or end > USER_END:
        sys.exit(f"Запись разрешена только в 0x{USER_START:06X}..0x{USER_END:06X}")
    first, last = addr // SECTOR * SECTOR, (end - 1) // SECTOR * SECTOR
    if addr % SECTOR:
        sys.exit("Адрес должен быть кратен 4 КБ: стирание идёт секторами")
    if not await flash_unprotect(f4):
        sys.exit("Защиту записи снять не удалось — флеш запись проигнорирует")
    t0 = time.time()
    for sec in range(first, last + 1, SECTOR):
        await f4.call([CMD_FLASH_ERASE, *struct.pack("<I", sec)[:3]], timeout=10)
        print(f"\r  стёрт сектор 0x{sec:06X}", end="", flush=True)
    print()
    # Пакеты без подтверждения (BLE всё равно доставляет их по порядку и без потерь),
    # каждые SYNC_EVERY — синхронизация запросом статуса: ждём его ответ и по пути
    # ловим ошибки записи (EE 46). Итог всё равно сверяется обратным чтением.
    SYNC_EVERY = 32
    pos, sent = 0, 0
    t1 = time.time()
    while pos < len(data):
        a = addr + pos
        n = min(WRITE_CHUNK, len(data) - pos, PAGE - a % PAGE)
        await f4.client.write_gatt_char(CTRL, bytes([CMD_FLASH_WRITE, *struct.pack("<I", a)[:3],
                                                     *data[pos:pos + n]]), response=False)
        pos += n
        sent += 1
        if sent % SYNC_EVERY == 0 or pos == len(data):
            await flash_sync(f4)
            rate = pos / max(time.time() - t1, 1e-3)
            print(f"\r  записано {pos}/{len(data)} байт, {rate / 1024:.2f} КБ/с ", end="", flush=True)
    print("\n  проверка...")
    import os
    import tempfile
    tmp = os.path.join(tempfile.gettempdir(), "f4_verify.bin")
    if not await flash_dump(f4, addr, len(data), tmp):
        sys.exit("Не удалось прочитать для проверки")
    back = open(tmp, "rb").read()
    os.remove(tmp)
    bad = sum(x != y for x, y in zip(back, data))
    if bad or len(back) != len(data):
        sys.exit(f"ПРОВЕРКА НЕ ПРОШЛА: различий {bad}")
    print(f"Записано и проверено {len(data)} байт за {time.time() - t0:.0f} с")


async def flash_sync(f4, timeout=10):
    """Запрос статуса после пачки записей: ответы 46 пропускаем, на EE — ошибка."""
    await f4.client.write_gatt_char(CTRL, bytes([CMD_FLASH_STATUS]), response=True)
    end = time.time() + timeout
    while True:
        resp = await asyncio.wait_for(f4.queue.get(), max(end - time.time(), 0.1))
        if resp[0] == RESP_ERROR:
            sys.exit(f"\nбраслет отклонил команду 0x{resp[1]:02X} — запись прервана")
        if resp[0] == CMD_FLASH_STATUS:
            return resp


def anim_bytes(path):
    """GIF -> "ANI1" w h n ms + кадры RGB565 BE; вписывается в экран с сохранением пропорций."""
    from PIL import Image, ImageSequence
    src = Image.open(path)
    k = min(SCREEN_W / src.width, SCREEN_H / src.height)
    w, h = max(1, round(src.width * k)), max(1, round(src.height * k))
    frames, delays = [], []
    for fr in ImageSequence.Iterator(src):
        frames.append(fr.convert("RGB").resize((w, h), Image.LANCZOS))
        delays.append(fr.info.get("duration") or 100)
    ms = round(sum(delays) / len(delays))
    out = bytearray(b"ANI1" + struct.pack("<HHHH", w, h, len(frames), ms))
    for im in frames:
        for y in range(h):
            for x in range(w):
                r, g, b = im.getpixel((x, y))
                out += struct.pack(">H", (r >> 3) << 11 | (g >> 2) << 5 | (b >> 3))
    return bytes(out), frames, (w, h, ms)


async def flash_dump(f4, addr, size, out, resume=False):
    """Пишет в файл по мере приёма: при обрыве полученное остаётся, --resume продолжает."""
    import os
    done = os.path.getsize(out) if resume and os.path.exists(out) else 0
    done = min(done, size)
    if done:
        print(f"Продолжаю с {done} байт")
    if done >= size:
        print("Уже снято полностью")
        return True
    t0, start = time.time(), done
    with open(out, "r+b" if done else "wb") as f:
        f.seek(done)
        f.truncate()
        await f4.call([CMD_FLASH_READ, *struct.pack("<I", addr + done)[:3],
                       *struct.pack("<I", size - done)[:3]])
        try:
            while done < size:
                evt = await asyncio.wait_for(f4.events.get(), 10)
                if evt[0] != EVT_FLASH:
                    continue
                chunk = evt[1:1 + size - done]
                f.write(chunk)
                done += len(chunk)
                if done % 4096 < 19 or done >= size:
                    f.flush()
                    rate = (done - start) / max(time.time() - t0, 1e-3)
                    left = (size - done) / max(rate, 1)
                    print(f"\r  {done}/{size} байт, {rate / 1024:.1f} КБ/с, осталось {left:.0f} с ",
                          end="", flush=True)
        except (asyncio.TimeoutError, Exception) as e:
            print(f"\nПоток остановился на {done} байтах: {e!r}")
            try:
                await f4.client.write_gatt_char(CTRL, bytes([CMD_FLASH_READ, 0, 0, 0, 0, 0, 0]),
                                                response=True)
            except Exception:
                pass
    print(f"\nВ файле {out}: {done} байт")
    return done >= size


def show_inputs(value, prev=None):
    pins = " ".join(f"P0.{p}={value >> p & 1}" for p in WATCHED)
    changed = "" if prev is None else "  <- " + ", ".join(
        f"P0.{p}" for p in WATCHED if (value ^ prev) >> p & 1)
    print(f"{time.strftime('%H:%M:%S')}  {pins}{changed}", flush=True)


async def watch_inputs(f4, seconds):
    # Начальное состояние читаем сами: уведомление приходит только при изменении.
    prev = struct.unpack("<I", await f4.peek(0x50000510, 4))[0]
    show_inputs(prev)
    print(f"Жмите кнопки, {seconds:.0f} с...")
    end = time.time() + seconds
    while time.time() < end:
        try:
            evt = await asyncio.wait_for(f4.events.get(), end - time.time())
        except asyncio.TimeoutError:
            break
        if evt[0] == EVT_INPUTS:
            value = struct.unpack_from("<I", evt, 1)[0]
            show_inputs(value, prev)
            prev = value


async def record_hr(f4, seconds, csv_path):
    samples = []
    await f4.call([CMD_HR, 1])
    print(f"Пульсометр включён, {seconds:.0f} с. Приложите палец или запястье к датчику.")
    end = time.time() + seconds
    last_seq = None
    try:
        while time.time() < end:
            try:
                evt = await asyncio.wait_for(f4.events.get(), end - time.time())
            except asyncio.TimeoutError:
                break
            if evt[0] != EVT_PPG:
                continue
            seq = evt[1]
            if last_seq is not None and seq != (last_seq + 1) & 0xFF:
                print(f"  потеряно пакетов: {(seq - last_seq - 1) & 0xFF}")
            last_seq = seq
            chunk = struct.unpack_from(f"<{(len(evt) - 2) // 2}H", evt, 2)
            samples += chunk
            bar = "#" * (sum(chunk) // len(chunk) * 50 // 1023)
            print(f"  {len(samples) / TICK_HZ:5.1f} с  min {min(chunk):4} max {max(chunk):4}  {bar}",
                  flush=True)
    finally:
        await f4.call([CMD_HR, 0])
        print("Пульсометр выключен.")
    if samples:
        print(f"Отсчётов: {len(samples)}, диапазон {min(samples)}..{max(samples)}")
    if csv_path and samples:
        with open(csv_path, "w") as f:
            f.write("t_s,adc\n")
            for i, v in enumerate(samples):
                f.write(f"{i / TICK_HZ:.2f},{v}\n")
        print(f"Записано {csv_path}")


if __name__ == "__main__":
    asyncio.run(main())
