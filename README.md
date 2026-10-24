# BadKernel

Kernel x86 escrito desde cero (NASM + C) cuyo unico trabajo es reproducir **Bad Apple!!**.
Sin GRUB, sin libc, sin interrupciones: todo por polling.

| Parte  | Como                                                                 |
|--------|----------------------------------------------------------------------|
| Boot   | MBR (INT 13h ext) -> stage2 (fuente ROM, A20, modo 13h, modo protegido) -> kernel en 0x8600 |
| Video  | VBE con framebuffer lineal (modo mas grande por defecto), cuadros 160x100 a 4 bpp en 16 grises, repinta solo lo que cambia; sin VBE cae a VGA 13h |
| Disco  | ATA PIO LBA28 (IDE), busca la cabecera "BADK" en LBA 2048            |
| Audio  | Intel HDA o AC'97 (drivers de PC BASIC), anillo DMA de 64 KiB, s16 48 kHz |
| Reloj  | la posicion del audio manda el video (sin audio: PIT por polling); salta cuadros si se atrasa |
| Debug  | COM1 115200 (`-serial stdio`), pantalla roja con texto en excepciones |

## Teclas
- **Ctrl+P**: siguiente resolucion de la lista (de mayor a menor; despues del ultimo vuelve al mayor).
- **Ctrl+O**: cambia la salida de audio. Primero "Todas" (todos los pines de salida activos), luego cada una por separado (Bocinas / Audifonos / Linea, segun lo que declare el codec HDA; en AC'97: Linea, Audifonos). La segunda linea verde de arriba muestra la salida activa. Si el codec solo tiene una salida, la tecla no hace nada.
  Nota: no probado en hardware real; en QEMU el codec HDA solo expone un pin, asi que el cambio real entre pines solo se pudo verificar en AC'97.
  Arriba, en verde, se ve `ancho x alto x bpp [n/total]`. Si un modo no sale en tu pantalla, sigue
  pulsando Ctrl+P a ciegas hasta uno que si.
- Las teclas se leen del controlador PS/2 (teclado interno de laptop o USB con emulacion de la BIOS).

## Uso
    make                                   # build/base.img (boot + kernel)
    make demo                              # video sintetico de 8 s para probar
    make bad VIDEO=badapple.mp4            # build/badkernel.img con tu video
    make run                               # QEMU + Intel HDA (QEMU_AUDIO=pipewire|pa|alsa)
    make run-ac97                          # QEMU + AC'97
    make run QEMU_FLAGS=                   # sin KVM

`make run` ya pasa `-serial stdio`: el kernel escribe ahi su registro (disco, tarjeta de audio,
cuadros mostrados/saltados, errores). Si algo falla, pega esa salida.
`tools/mkbad.py -h` tiene las opciones (`--w --h --bpp --fps --av-delay --aspect --vf`).

## En hardware real
`dd if=build/badkernel.img of=/dev/sdX` solo funciona si la BIOS ve el disco por puertos IDE
legacy (modo IDE/compat, no AHCI/USB). El audio usa HDA/AC'97 reales; sin tarjeta reproduce solo
video. Siguiente paso: lectura por AHCI/USB (hay codigo en PC BASIC: blk.c y usb.c).
