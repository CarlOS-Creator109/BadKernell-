# BadKernel

Kernel x86 escrito desde cero (NASM + C) cuyo unico trabajo es reproducir **Bad Apple!!**.
Sin GRUB, sin libc, sin interrupciones: todo por polling.

| Parte  | Como                                                                 |
|--------|----------------------------------------------------------------------|
| Boot   | MBR (INT 13h ext) -> stage2 (fuente ROM, A20, modo 13h, modo protegido) -> kernel en 0x8600 |
| Video  | VGA 13h 320x200, paleta de grises, cuadros 160x100 a 4 bpp           |
| Disco  | ATA PIO LBA28 (IDE), busca la cabecera "BADK" en LBA 2048            |
| Audio  | Intel HDA o AC'97 (drivers de PC BASIC), anillo DMA de 64 KiB, s16 48 kHz |
| Reloj  | la posicion del audio manda el video (sin audio: PIT por polling); salta cuadros si se atrasa |
| Debug  | COM1 115200 (`-serial stdio`), pantalla roja con texto en excepciones |

## Uso
    make                                   # build/base.img (boot + kernel)
    make demo                              # video sintetico de 8 s para probar
    make bad VIDEO=badapple.mp4            # build/badkernel.img con tu video
    make run                               # QEMU + Intel HDA (QEMU_AUDIO=pipewire|pa|alsa)
    make run-ac97                          # QEMU + AC'97
    make run QEMU_FLAGS=                   # sin KVM

`make run` ya pasa `-serial stdio`: el kernel escribe ahi su registro (disco, tarjeta de audio,
cuadros mostrados/saltados, errores). Si algo falla, pega esa salida.
`tools/mkbad.py -h` tiene las opciones (`--w --h --bpp --fps --av-delay --vf`).

## En hardware real
`dd if=build/badkernel.img of=/dev/sdX` solo funciona si la BIOS ve el disco por puertos IDE
legacy (modo IDE/compat, no AHCI/USB). El audio usa HDA/AC'97 reales; sin tarjeta reproduce solo
video. Siguiente paso: lectura por AHCI/USB (hay codigo en PC BASIC: blk.c y usb.c).
