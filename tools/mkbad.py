#!/usr/bin/env python3
"""mkbad.py - empaqueta un video en una imagen de disco booteable de BadKernel.

    mkbad.py build/base.img bad_apple.mp4 -o build/badkernel.img

Formato (todo little-endian), a partir de LBA 2048 (1 MiB):
    sector 0 : cabecera  'BADK' + w h bpp fps frames frame_sectors video_lba
                         audio_rate audio_lba audio_bytes  (u32)  + av_delay_ms (s32)
    sector 1+: cuadros, cada uno ocupa frame_sectors sectores; pixeles en gris de
               bpp bits, empaquetados MSB primero, fila tras fila
    luego    : audio PCM s16le mono a 48000 Hz
"""
import argparse
import os
import struct
import subprocess
import sys

SECTOR = 512
DATA_LBA = 2048


def read_exact(stream, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return bytes(buf)


def pack_frame(gray, bpp, table):
    """gray: bytes 8 bpp -> bytes empaquetados a bpp bits (sin numpy: truco con enteros grandes)."""
    q = gray.translate(table)
    if bpp == 8:
        return q
    ppb = 8 // bpp
    acc = int.from_bytes(q[0::ppb], "big")
    for k in range(1, ppb):
        acc = (acc << bpp) | int.from_bytes(q[k::ppb], "big")
    return acc.to_bytes(len(q) // ppb, "big")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base", help="imagen base (boot + stage2 + kernel) de 'make'")
    ap.add_argument("video", help="video de entrada (lo que lea ffmpeg)")
    ap.add_argument("-o", "--out", default="badkernel.img")
    ap.add_argument("--w", type=int, default=160, help="ancho del cuadro (default 160)")
    ap.add_argument("--h", type=int, default=100, help="alto del cuadro (default 100)")
    ap.add_argument("--bpp", type=int, default=4, choices=(1, 2, 4), help="bits de gris por pixel")
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--aspect", default="4:3", help="relacion de aspecto con que se muestra la imagen (default 4:3)")
    ap.add_argument("--no-audio", action="store_true")
    ap.add_argument("--av-delay", type=int, default=0, help="ms de retraso del video respecto al audio (puede ser negativo)")
    ap.add_argument("--vf", default="", help="filtro ffmpeg extra antes del escalado (ej: crop=480:360:0:0)")
    ap.add_argument("--max-seconds", type=float, default=0, help="cortar el video (para pruebas)")
    args = ap.parse_args()

    if (args.w * args.h * args.bpp) % 8:
        sys.exit("w*h*bpp tiene que ser multiplo de 8")
    if args.w > 320 or args.h > 200:
        sys.exit("el modo 13h es de 320x200 como maximo")

    base = open(args.base, "rb").read()
    if len(base) != DATA_LBA * SECTOR:
        sys.exit(f"{args.base}: tamano inesperado ({len(base)}), esperaba {DATA_LBA * SECTOR}")

    levels = 1 << args.bpp
    table = bytes((v * (levels - 1) + 127) // 255 for v in range(256))
    raw_size = args.w * args.h
    frame_sectors = (raw_size * args.bpp // 8 + SECTOR - 1) // SECTOR
    if frame_sectors > 64:
        sys.exit("cuadro demasiado grande para el buffer del kernel (max 64 sectores)")
    frame_bytes = frame_sectors * SECTOR

    vf = (args.vf + "," if args.vf else "") + \
        f"fps={args.fps},scale={args.w}:{args.h}:flags=area,format=gray"
    cmd = ["ffmpeg", "-v", "error", "-i", args.video]
    if args.max_seconds:
        cmd += ["-t", str(args.max_seconds)]
    cmd += ["-an", "-vf", vf, "-f", "rawvideo", "-pix_fmt", "gray", "-"]

    out = open(args.out, "wb")
    out.write(base)
    out.write(b"\0" * SECTOR)                       # cabecera (se reescribe al final)
    video_lba = DATA_LBA + 1

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    frames = 0
    while True:
        gray = read_exact(proc.stdout, raw_size)
        if len(gray) < raw_size:
            break
        packed = pack_frame(gray, args.bpp, table)
        out.write(packed + b"\0" * (frame_bytes - len(packed)))
        frames += 1
        if frames % 500 == 0:
            print(f"  {frames} cuadros...", file=sys.stderr)
    if proc.wait() != 0 or frames == 0:
        sys.exit("ffmpeg no pudo leer el video")

    audio_lba = video_lba + frames * frame_sectors
    audio = b""
    rate = 0
    if not args.no_audio:
        rate = 48000
        acmd = ["ffmpeg", "-v", "error", "-i", args.video]
        if args.max_seconds:
            acmd += ["-t", str(args.max_seconds)]
        acmd += ["-vn", "-ac", "1", "-ar", str(rate), "-f", "s16le", "-"]
        res = subprocess.run(acmd, stdout=subprocess.PIPE)
        audio = res.stdout
        if not audio:
            print("aviso: el video no tiene audio, se sigue sin sonido", file=sys.stderr)
            rate = 0
    audio_bytes = len(audio)
    out.write(audio + b"\0" * (-audio_bytes % SECTOR))

    an, ad = (int(v) for v in args.aspect.split(":"))
    hdr = struct.pack("<4s10IiII", b"BADK", args.w, args.h, args.bpp, args.fps, frames,
                      frame_sectors, video_lba, rate, audio_lba, audio_bytes, args.av_delay, an, ad)
    out.seek(DATA_LBA * SECTOR)
    out.write(hdr.ljust(SECTOR, b"\0"))
    out.close()

    total = os.path.getsize(args.out)
    print(f"{args.out}: {frames} cuadros {args.w}x{args.h} {args.bpp}bpp @{args.fps}fps, "
          f"audio {audio_bytes} bytes @{rate}Hz, {total / 1048576:.1f} MiB")


if __name__ == "__main__":
    main()
