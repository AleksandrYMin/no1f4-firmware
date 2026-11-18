#!/usr/bin/env python3
"""No.1 F4 (nRF51422 + S110) — извлечение/замена картинок и сборка DFU-пакета.

Формат картинок в прошивке: 1 бит/пиксель, построчно, старший бит слева,
без заголовка; ширина округляется вверх до кратной 8. Картинки хранятся
повёрнутыми на 90° относительно того, как видны на экране; по умолчанию
скрипт поворачивает их, чтобы PNG выглядел «как на браслете».
W и H задаются в формате хранения (W — ширина строки в прошивке:
64 для крупных иконок, 32 для погоды, 16 для надписей).

Примеры:
  python3 f4gfx.py extract fw.bin 0x30A40 64 64 icon.png
  python3 f4gfx.py replace fw.bin 0x30A40 64 64 my.png -o fw_mod.bin
  python3 f4gfx.py pack fw_mod.bin -o no1f4_mod.zip
  python3 f4gfx.py info fw.bin

Требуется: pip install pillow
"""
import argparse
import json
import struct
import sys
import zipfile

BASE = 0x18000          # адрес загрузки приложения (S110 v8)
SD_REQ = 0x0064         # S110 v8.0.0
APP_MAX = 0x3C000 - BASE  # граница перед загрузчиком (на всякий случай)


def crc16_ccitt(data, crc=0xFFFF):
    """CRC16 из Nordic SDK (crc16.c), используется legacy DFU."""
    for b in data:
        crc = ((crc >> 8) & 0xFF) | ((crc << 8) & 0xFFFF)
        crc ^= b
        crc ^= (crc & 0xFF) >> 4
        crc ^= (crc << 8) << 4 & 0xFFFF
        crc ^= ((crc & 0xFF) << 4) << 1 & 0xFFFF
    return crc & 0xFFFF


def span(addr, w, h):
    rb = (w + 7) // 8
    off = addr - BASE
    return off, rb, rb * h


def check(fw, addr, w, h):
    off, rb, n = span(addr, w, h)
    if off < 0 or off + n > len(fw):
        sys.exit(f"Ошибка: область 0x{addr:X}..0x{addr + n:X} вне образа")
    return off, rb, n


def to_image(fw, addr, w, h, raw):
    from PIL import Image
    off, rb, _ = check(fw, addr, w, h)
    im = Image.new("1", (w, h), 0)
    px = im.load()
    for y in range(h):
        for x in range(w):
            if fw[off + y * rb + x // 8] >> (7 - x % 8) & 1:
                px[x, y] = 1
    return im if raw else im.rotate(90, expand=True)


def from_image(path, w, h, raw, threshold=128, invert=False):
    from PIL import Image
    im = Image.open(path).convert("L")
    if not raw:
        im = im.rotate(-90, expand=True)
    if im.size != (w, h):
        need = (w, h) if raw else (h, w)
        sys.exit(f"Ошибка: размер PNG {Image.open(path).size}, нужен {need} "
                 f"(ширина x высота{' как на экране' if not raw else ''})")
    rb = (w + 7) // 8
    out = bytearray(rb * h)
    px = im.load()
    for y in range(h):
        for x in range(w):
            on = px[x, y] >= threshold
            if invert:
                on = not on
            if on:
                out[y * rb + x // 8] |= 0x80 >> (x % 8)
    return bytes(out)


def cmd_extract(a):
    fw = open(a.fw, "rb").read()
    to_image(fw, a.addr, a.w, a.h, a.raw).convert("L").save(a.out)
    print(f"Сохранено {a.out}")


def cmd_replace(a):
    fw = bytearray(open(a.fw, "rb").read())
    off, rb, n = check(fw, a.addr, a.w, a.h)
    new = from_image(a.png, a.w, a.h, a.raw, a.threshold, a.invert)
    changed = sum(1 for i in range(n) if fw[off + i] != new[i])
    fw[off:off + n] = new
    out = a.out or a.fw.replace(".bin", "_mod.bin")
    open(out, "wb").write(fw)
    print(f"Заменено {n} байт по 0x{a.addr:X} (изменилось {changed}). "
          f"CRC16 = {crc16_ccitt(fw)}. Записано {out}")


def make_dat(crc):
    # device_type, device_rev, app_version, sd_req count, sd_req, crc16
    return struct.pack("<HHIHHH", 0xFFFF, 0xFFFF, 0xFFFFFFFF, 1, SD_REQ, crc)


def cmd_pack(a):
    fw = open(a.fw, "rb").read()
    if len(fw) > APP_MAX:
        sys.exit("Ошибка: образ больше области приложения")
    crc = crc16_ccitt(fw)
    name = "nrf51422_xxac_s110"
    manifest = {"manifest": {"application": {
        "bin_file": name + ".bin", "dat_file": name + ".dat",
        "init_packet_data": {
            "application_version": 0xFFFFFFFF, "device_revision": 0xFFFF,
            "device_type": 0xFFFF, "firmware_crc16": crc,
            "softdevice_req": [SD_REQ]}},
        "dfu_version": 0.5}}
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(name + ".bin", fw)
        z.writestr(name + ".dat", make_dat(crc))
        z.writestr("manifest.json", json.dumps(manifest, indent=4))
    print(f"DFU-пакет {a.out}: {len(fw)} байт, CRC16 = {crc}")


def cmd_info(a):
    fw = open(a.fw, "rb").read()
    sp, rst = struct.unpack_from("<II", fw, 0)
    print(f"Размер {len(fw)} байт, 0x{BASE:X}..0x{BASE + len(fw):X}")
    print(f"SP 0x{sp:08X}, Reset 0x{rst:08X}, CRC16 {crc16_ccitt(fw)}")


def num(s):
    return int(s, 0)


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    for name in ("extract", "replace"):
        s = sub.add_parser(name)
        s.add_argument("fw")
        s.add_argument("addr", type=num)
        s.add_argument("w", type=num, help="ширина в формате хранения")
        s.add_argument("h", type=num, help="высота в формате хранения")
        s.add_argument("--raw", action="store_true",
                       help="без поворота (как хранится в прошивке)")
        if name == "extract":
            s.add_argument("out")
        else:
            s.add_argument("png")
            s.add_argument("-o", "--out")
            s.add_argument("--threshold", type=int, default=128)
            s.add_argument("--invert", action="store_true")
    s = sub.add_parser("pack")
    s.add_argument("fw")
    s.add_argument("-o", "--out", default="no1f4_mod.zip")
    s = sub.add_parser("info")
    s.add_argument("fw")
    a = p.parse_args()
    {"extract": cmd_extract, "replace": cmd_replace,
     "pack": cmd_pack, "info": cmd_info}[a.cmd](a)


if __name__ == "__main__":
    main()
