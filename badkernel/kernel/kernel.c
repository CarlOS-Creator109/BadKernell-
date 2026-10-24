/*
 * BadKernel - un kernel desde cero que solo sabe reproducir Bad Apple!!
 *
 *  - modo protegido de 32 bits, sin interrupciones (todo por polling)
 *  - video: VGA modo 13h (320x200), paleta de grises, 1/2/4 bits por pixel
 *  - disco: ATA PIO (IDE) en LBA28, busca solo los datos "BADK"
 *  - audio: Intel HDA o AC'97 (drivers de PC BASIC), anillo DMA de 64 KiB,
 *           PCM s16 a 48 kHz (mono en disco, se duplica a estereo)
 *  - reloj: la posicion del audio manda; sin audio, el PIT leido por polling
 *  - depuracion: puerto serie COM1 (115200 8N1)
 */
#include "kernel.h"

#ifndef DATA_LBA
#define DATA_LBA 2048
#endif

#define MAGIC     0x4B444142u      /* "BADK" */
#define PIT_HZ    1193182u
#define AUDIO_HZ  48000u

#define RING      ((u8 *)0x200000)     /* anillo DMA, alineado a 64 KiB */
#define RINGSZ    0x10000u
#define CHUNK     0x4000u              /* bytes estereo por relleno (4096 muestras) */
#define AUDTMP    ((u8 *)0x210000)
#define FRAMEBUF  ((u8 *)0x220000)     /* hasta 64 sectores por cuadro */
#define BACKBUF   ((u8 *)0x230000)
#define FONT      ((const u8 *)0x70000)
#define VGAMEM    0xA0000u

#define COL_WHITE 255
#define COL_RED   254
#define COL_GREEN 253

struct hdr {
    u32 magic;
    u32 w, h, bpp, fps, frames, frame_sectors, video_lba;
    u32 audio_rate, audio_lba, audio_bytes;
    s32 av_delay_ms;
};

/* ---------------------------------------------------------------- memoria */

void *memcpy(void *d, const void *s, u32 n)
{
    void *r = d;
    __asm__ volatile("cld; rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory");
    return r;
}

void *memset(void *d, int c, u32 n)
{
    void *r = d;
    __asm__ volatile("cld; rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}

static void copy32(void *d, const void *s, u32 dwords)
{
    __asm__ volatile("cld; rep movsl" : "+D"(d), "+S"(s), "+c"(dwords) :: "memory");
}

static inline void cpu_pause(void) { __asm__ volatile("pause"); }

/* -------------------------------------------------------- puerto serie */

static void serial_init(void)
{
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x80);
    outb(0x3F8, 0x01);          /* 115200 */
    outb(0x3F9, 0x00);
    outb(0x3FB, 0x03);
    outb(0x3FA, 0xC7);
    outb(0x3FC, 0x0B);
}

static void sputc(char c)
{
    for (u32 i = 0; i < 100000; i++)
        if (inb(0x3FD) & 0x20) break;
    outb(0x3F8, (u8)c);
}

void sputs(const char *s)
{
    for (; *s; s++) {
        if (*s == '\n') sputc('\r');
        sputc(*s);
    }
}

void sdec(u32 v)
{
    char b[12]; int n = 0;
    if (!v) b[n++] = '0';
    while (v) { b[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) sputc(b[--n]);
}

void shex(u32 v)
{
    sputs("0x");
    for (int i = 28; i >= 0; i -= 4) sputc("0123456789ABCDEF"[(v >> i) & 15]);
}

/* ---------------------------------------------------------------- video */

static void dac(u8 i, u8 r, u8 g, u8 b)
{
    outb(0x3C8, i);
    outb(0x3C9, r);
    outb(0x3C9, g);
    outb(0x3C9, b);
}

static void palette_init(u32 bpp)
{
    u32 n = 1u << bpp;
    for (u32 i = 0; i < n; i++) {
        u8 v = (u8)(i * 63 / (n - 1));
        dac((u8)i, v, v, v);
    }
    dac(COL_WHITE, 63, 63, 63);
    dac(COL_RED, 63, 12, 12);
    dac(COL_GREEN, 20, 63, 20);
}

static void cls(void) { memset((void *)VGAMEM, 0, 64000); }

static void text(u32 x, u32 y, const char *s, u8 col)
{
    volatile u8 *v = (volatile u8 *)VGAMEM;
    for (; *s && x + 8 <= 320; s++, x += 8) {
        const u8 *g = FONT + (u32)(u8)*s * 8;
        for (u32 r = 0; r < 8; r++)
            for (u32 c = 0; c < 8; c++)
                if (g[r] & (0x80 >> c))
                    v[(y + r) * 320 + x + c] = col;
    }
}

static void fatal(const char *msg)
{
    sputs("FATAL: "); sputs(msg); sputs("\n");
    cls();
    text(8, 8, "BADKERNEL: ERROR", COL_RED);
    text(8, 24, msg, COL_WHITE);
    for (;;) { __asm__ volatile("cli; hlt"); }
}

static u16 xmap[320];
static u16 ymap[200];

static void blit(const u8 *src, u32 W, u32 bpp)
{
    u32 mask = (1u << bpp) - 1;
    u32 prev = 0xFFFFFFFFu;
    for (u32 y = 0; y < 200; y++) {
        u8 *row = BACKBUF + y * 320;
        u32 sy = ymap[y];
        if (sy == prev) {
            memcpy(row, row - 320, 320);
            continue;
        }
        prev = sy;
        u32 base = sy * W;
        for (u32 x = 0; x < 320; x++) {
            u32 idx = (base + xmap[x]) * bpp;
            row[x] = (u8)((src[idx >> 3] >> (8 - bpp - (idx & 7))) & mask);
        }
    }
    copy32((void *)VGAMEM, BACKBUF, 16000);
}

/* ------------------------------------------------------------------ PIT */

static u32 pit_acc;
static u16 pit_last;

static void pit_init(void)
{
    outb(0x43, 0x34);           /* canal 0, lo/hi, modo 2 */
    outb(0x40, 0);
    outb(0x40, 0);              /* recarga 65536 */
    outb(0x43, 0x00);
    u16 lo = inb(0x40);
    u16 hi = inb(0x40);
    pit_last = (u16)(lo | (hi << 8));
    pit_acc = 0;
}

/* ticks del PIT acumulados; hay que llamarla mas de una vez cada 54 ms */
static u32 pit_now(void)
{
    outb(0x43, 0x00);
    u16 lo = inb(0x40);
    u16 hi = inb(0x40);
    u16 c = (u16)(lo | (hi << 8));
    pit_acc += (u16)(pit_last - c);
    pit_last = c;
    return pit_acc;
}

/* espera con tope de iteraciones: si el PIT no avanzara, no nos colgamos */
void delay_ms(int ms)
{
    u32 end = pit_now() + (u32)ms * 1193;
    for (u32 i = 0; i < 20000u * (u32)ms; i++) {
        if ((s32)(pit_now() - end) >= 0) break;
        cpu_pause();
    }
}

/* ------------------------------------------------------------------ PCI */

u32 pci_read32(int bus, int dev, int fn, int off)
{
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | ((u32)off & 0xFC));
    return inl(0xCFC);
}

void pci_write32(int bus, int dev, int fn, int off, u32 v)
{
    outl(0xCF8, 0x80000000u | ((u32)bus << 16) | ((u32)dev << 11) | ((u32)fn << 8) | ((u32)off & 0xFC));
    outl(0xCFC, v);
}

int pci_find(int cls, int sub, int prog, int nth, int *bus, int *dev, int *fn)
{
    for (int b = 0; b < 16; b++) {
        for (int d = 0; d < 32; d++) {
            for (int f = 0; f < 8; f++) {
                if ((pci_read32(b, d, f, 0) & 0xFFFF) == 0xFFFF) {
                    if (f == 0) break;
                    continue;
                }
                u32 cr = pci_read32(b, d, f, 8);
                if ((int)(cr >> 24) == cls && (int)((cr >> 16) & 0xFF) == sub &&
                    (prog < 0 || (int)((cr >> 8) & 0xFF) == prog)) {
                    if (nth-- == 0) { *bus = b; *dev = d; *fn = f; return 1; }
                }
                if (f == 0 && !(pci_read32(b, d, 0, 0x0C) & 0x00800000u))
                    break;                              /* un solo dispositivo */
            }
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ ATA */

static u16 ata_base = 0x1F0;
static u8  ata_slave = 0;

static int ata_wait_drq(u16 b)
{
    for (u32 i = 0; i < 400000; i++) {
        u8 s = inb(b + 7);
        if (s & 0x80) continue;
        if (s & 0x21) return 0;     /* ERR o DF */
        if (s & 0x08) return 1;     /* DRQ */
    }
    return 0;
}

static int ata_read(u32 lba, u32 count, void *buf)
{
    u16 b = ata_base;
    u16 *p = (u16 *)buf;

    for (u32 i = 0; ; i++) {
        if (i > 400000) { sputs("ata: busy timeout status="); shex(inb(b + 7)); sputs("\n"); return -1; }
        if (!(inb(b + 7) & 0x80)) break;
    }
    outb(b + 6, (u8)(0xE0 | (ata_slave << 4) | ((lba >> 24) & 0x0F)));
    outb(b + 2, (u8)count);
    outb(b + 3, (u8)lba);
    outb(b + 4, (u8)(lba >> 8));
    outb(b + 5, (u8)(lba >> 16));
    outb(b + 7, 0x20);              /* READ SECTORS */

    for (u32 s = 0; s < count; s++) {
        inb(b + 0x206); inb(b + 0x206); inb(b + 0x206); inb(b + 0x206);
        if (!ata_wait_drq(b)) {
            sputs("ata: fallo lba="); sdec(lba); sputs(" sector "); sdec(s); sputs("/"); sdec(count);
            sputs(" status="); shex(inb(b + 7)); sputs(" err="); shex(inb(b + 1)); sputs("\n");
            return -1;
        }
        u32 words = 256;
        __asm__ volatile("cld; rep insw" : "+D"(p), "+c"(words) : "d"(b) : "memory");
    }
    return 0;
}

static int find_disk(struct hdr *h)
{
    static const u16 bases[2] = { 0x1F0, 0x170 };
    u8 *sec = FRAMEBUF;
    for (u32 bi = 0; bi < 2; bi++) {
        for (u32 sl = 0; sl < 2; sl++) {
            ata_base = bases[bi];
            ata_slave = (u8)sl;
            u8 st = inb(ata_base + 7);
            sputs("ata: sondeo base "); shex(ata_base); sputs(" esclavo "); sdec(sl);
            sputs(" status="); shex(st); sputs("\n");
            if (st == 0xFF) continue;                    /* bus flotante */
            if (ata_read(DATA_LBA, 1, sec) < 0) continue;
            struct hdr *t = (struct hdr *)sec;
            if (t->magic != MAGIC) continue;
            memcpy(h, t, sizeof(*h));
            sputs("disco: base "); shex(ata_base); sputs(" esclavo "); sdec(sl); sputs("\n");
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- audio */

static struct hdr H;
static int snd;                 /* 0 ninguno, 1 HDA, 2 AC'97 */
static u32 audio_off;           /* bytes de audio mono ya leidos del disco */
static u32 a_written;           /* bytes estereo escritos al anillo */
static u32 a_played;            /* bytes estereo que el DMA ya consumio */
static u32 a_lastpos;

static u32 snd_pos(void) { return snd == 1 ? hda_pos() : ac97_pos(); }

static void fill_chunk(void)
{
    u32 want = CHUNK / 2;                                   /* bytes mono por relleno */
    u32 left = H.audio_bytes > audio_off ? H.audio_bytes - audio_off : 0;
    u32 n = left < want ? left : want;

    if (n && ata_read(H.audio_lba + audio_off / 512, (n + 511) / 512, AUDTMP) < 0)
        fatal("error de lectura de audio");
    if (n < want) memset(AUDTMP + n, 0, want - n);          /* silencio */
    audio_off += n;

    const u16 *s = (const u16 *)AUDTMP;
    u32 *d = (u32 *)(RING + (a_written & (RINGSZ - 1)));
    for (u32 i = 0; i < want / 2; i++) {
        u32 v = s[i];
        d[i] = v | (v << 16);                               /* mono -> estereo */
    }
    a_written += CHUNK;
}

static void audio_poll(void)
{
    u32 pos = snd_pos() & (RINGSZ - 1);
    u32 d = (pos - a_lastpos) & (RINGSZ - 1);
    if (d < RINGSZ / 2) {                                   /* un retroceso es ruido del driver */
        a_played += d;
        a_lastpos = pos;
    }
    while (a_written - a_played + CHUNK + 4096 <= RINGSZ)
        fill_chunk();
}

static void audio_start(void)
{
    memset(RING, 0, RINGSZ);
    a_written = a_played = a_lastpos = 0;
    while (a_written + CHUNK + 4096 <= RINGSZ)              /* prellenado (antes de arrancar) */
        fill_chunk();
    if (snd == 1) hda_start(RING, RINGSZ); else ac97_start(RING, RINGSZ);
}

static void audio_stop(void)
{
    if (snd == 1) hda_stop(); else ac97_stop();
}

/* ------------------------------------------------------------ excepciones */

struct idt_entry { u16 lo, sel; u8 zero, flags; u16 hi; } __attribute__((packed));
static struct idt_entry idt[32];
extern u32 exc_stub_table[32];

static void idt_init(void)
{
    for (u32 i = 0; i < 32; i++) {
        u32 a = exc_stub_table[i];
        idt[i].lo = (u16)(a & 0xFFFF);
        idt[i].sel = 0x08;
        idt[i].zero = 0;
        idt[i].flags = 0x8E;
        idt[i].hi = (u16)(a >> 16);
    }
    struct { u16 lim; u32 base; } __attribute__((packed)) p = { sizeof(idt) - 1, (u32)idt };
    __asm__ volatile("lidt %0" :: "m"(p));
}

void exc_handler(u32 *f)
{
    u32 v = f[0];
    int has_err = (v == 8 || (v >= 10 && v <= 14) || v == 17 || v == 21 || v == 29 || v == 30);
    u32 err = has_err ? f[1] : 0;
    u32 eip = has_err ? f[2] : f[1];
    sputs("EXCEPCION "); sdec(v); sputs(" err="); shex(err); sputs(" eip="); shex(eip); sputs("\n");
    cls();
    char m[] = "EXC 00 EIP 00000000";
    m[4] = (char)('0' + v / 10); m[5] = (char)('0' + v % 10);
    for (int i = 0; i < 8; i++) m[11 + i] = "0123456789ABCDEF"[(eip >> (28 - 4 * i)) & 15];
    text(8, 8, "BADKERNEL: EXCEPCION", COL_RED);
    text(8, 24, m, COL_WHITE);
}

/* ---------------------------------------------------------------- kmain */

static void show_frame(u32 f)
{
    if (ata_read(H.video_lba + f * H.frame_sectors, H.frame_sectors, FRAMEBUF) < 0)
        fatal("error de lectura de video");
    blit(FRAMEBUF, H.w, H.bpp);
}

static u32 pit_t0;

/* cuadro que deberia verse ahora (-1 si todavia no toca el primero) */
static s32 clock_frame(void)
{
    if (snd) {
        s32 p = (s32)(a_played >> 2) - H.av_delay_ms * 48;      /* 48 muestras por ms */
        return p < 0 ? -1 : (s32)((u32)p / (AUDIO_HZ / H.fps));
    }
    s32 e = (s32)(pit_now() - pit_t0) - H.av_delay_ms * 1193;
    return e < 0 ? -1 : (s32)((u32)e / (PIT_HZ / H.fps));
}

void kmain(void)
{
    serial_init();
    sputs("\nBadKernel 0.2\n");
    idt_init();
    pit_init();
    palette_init(4);
    cls();
    text(8, 8, "BADKERNEL 0.2", COL_WHITE);
    text(8, 24, "buscando disco...", COL_GREEN);

    if (!find_disk(&H)) fatal("no hay datos BADK en discos IDE");
    if (H.bpp != 1 && H.bpp != 2 && H.bpp != 4) fatal("bpp no soportado");
    if (H.w == 0 || H.w > 320 || H.h == 0 || H.h > 200) fatal("resolucion invalida");
    if (H.fps == 0 || H.fps > 120) fatal("fps invalido");
    if (H.frame_sectors == 0 || H.frame_sectors > 64) fatal("cuadro demasiado grande");
    if (H.frame_sectors * 512 * 8 < H.w * H.h * H.bpp) fatal("cuadro mas chico que la imagen");

    sputs("video "); sdec(H.w); sputs("x"); sdec(H.h); sputs(" bpp "); sdec(H.bpp);
    sputs(" fps "); sdec(H.fps); sputs(" cuadros "); sdec(H.frames); sputs("\n");

    for (u32 x = 0; x < 320; x++) xmap[x] = (u16)(x * H.w / 320);
    for (u32 y = 0; y < 200; y++) ymap[y] = (u16)(y * H.h / 200);
    palette_init(H.bpp);

    const char *sinfo = "sin audio";
    if (H.audio_bytes && H.audio_rate == AUDIO_HZ && (AUDIO_HZ % H.fps) == 0) {
        text(8, 40, "iniciando audio...", COL_GREEN);
        if (hda_open())       { snd = 1; sinfo = hda_info(); }
        else if (ac97_open()) { snd = 2; sinfo = ac97_info(); }
        sputs(snd ? sinfo : "no hay tarjeta HDA ni AC97: video sin audio"); sputs("\n");
    } else if (H.audio_bytes) {
        sputs("audio no es s16 mono a 48000 Hz (o fps no divide 48000): se ignora\n");
    }

    cls();
    text(8, 8, "BADKERNEL 0.2", COL_WHITE);
    text(8, 24, snd == 1 ? "audio: Intel HDA" : snd == 2 ? "audio: AC'97" : "sin audio", COL_GREEN);
    delay_ms(1200);
    cls();

    if (snd) audio_start();
    pit_t0 = pit_now();

    s32 last = -1;
    u32 shown = 0, skipped = 0;
    for (;;) {
        if (snd) audio_poll();
        s32 c = clock_frame();
        if (c < 0 || c == last) { cpu_pause(); continue; }
        if ((u32)c >= H.frames) break;
        skipped += (u32)(c - last - 1);
        show_frame((u32)c);
        last = c;
        shown++;
        if (shown % 300 == 1) {
            sputs("cuadro "); sdec((u32)c); sputs(" audio "); sdec(a_played >> 2); sputs("\n");
        }
    }

    if (snd) audio_stop();
    sputs("fin: mostrados "); sdec(shown); sputs(" saltados "); sdec(skipped); sputs("\n");
    cls();
    text(8, 8, "FIN", COL_WHITE);
    text(8, 24, "gracias por ver Bad Apple!!", COL_WHITE);
    for (;;) { __asm__ volatile("cli; hlt"); }
}
