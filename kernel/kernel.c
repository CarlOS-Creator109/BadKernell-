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
#define PREV      ((u8 *)0x240000)     /* ultimo cuadro dibujado (para repintar por diferencias) */
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
    u32 aspect_n, aspect_d;         /* relacion de aspecto de la imagen (0 = 4:3) */
};
static struct hdr H;

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


/* -------------------------------------------------------------- VBE / LFB */

struct rm_regs { u32 eax, ebx, ecx, edx, esi, edi, es, ds, eflags; };
#define RMR ((volatile struct rm_regs *)0x7000)
extern void bios_int(u32 n);

static u32 gdt_tab[10] = {
    0, 0,
    0x0000FFFF, 0x00CF9A00,     /* 0x08 codigo 32 bits */
    0x0000FFFF, 0x00CF9200,     /* 0x10 datos 32 bits */
    0x0000FFFF, 0x00009A00,     /* 0x18 codigo 16 bits (para volver a modo real) */
    0x0000FFFF, 0x00009200,     /* 0x20 datos 16 bits */
};

static void gdt_init(void)
{
    struct { u16 lim; u32 base; } __attribute__((packed)) p = { sizeof(gdt_tab) - 1, (u32)gdt_tab };
    __asm__ volatile("lgdt %0\n\t"
                     "ljmp $0x08, $1f\n"
                     "1:\n\t"
                     "mov $0x10, %%ax\n\t"
                     "mov %%ax, %%ds\n\tmov %%ax, %%es\n\tmov %%ax, %%fs\n\t"
                     "mov %%ax, %%gs\n\tmov %%ax, %%ss"
                     :: "m"(p) : "eax", "memory");
}

static void rm_call(u32 n, u32 eax, u32 ebx, u32 ecx, u32 edi, u32 es)
{
    memset((void *)0x7000, 0, sizeof(struct rm_regs));
    RMR->eax = eax; RMR->ebx = ebx; RMR->ecx = ecx; RMR->edi = edi; RMR->es = es;
    bios_int(n);
}

struct vmode { u16 mode, w, h; u8 bpp, rs, rp, gs, gp, bs, bp; u32 fb, pitch; };
static struct vmode vm[48];
static int nvm, cur_vm = -1, vbe_on;
static struct { u8 *fb; u32 w, h, pitch, bytes; struct vmode *m; } G;
static u32 lvl_pal[16];
static u16 xs[321], ys[201];
static u32 vx, vy, dw, dh, bar_h, fscale;

static char *utoa(char *o, u32 v)
{
    char t[11]; int n = 0;
    if (!v) t[n++] = '0';
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *o++ = t[--n];
    return o;
}

static char *scat(char *o, const char *s) { while (*s) *o++ = *s++; return o; }

static void vbe_enum(void)
{
    nvm = 0;
    memset((void *)0x3000, 0, 512);
    memcpy((void *)0x3000, "VBE2", 4);
    rm_call(0x10, 0x4F00, 0, 0, 0, 0x300);
    if ((RMR->eax & 0xFFFF) != 0x004F || *(volatile u32 *)0x3000 != 0x41534556u) {
        sputs("vbe: la BIOS no ofrece VBE\n");
        return;
    }
    u32 ptr = *(volatile u32 *)(0x3000 + 0x0E);
    const volatile u16 *list = (const volatile u16 *)(((ptr >> 16) << 4) + (ptr & 0xFFFF));
    for (int i = 0; i < 256 && list[i] != 0xFFFF; i++) {
        u16 mode = list[i];
        rm_call(0x10, 0x4F01, 0, mode, 0, 0x320);
        if ((RMR->eax & 0xFFFF) != 0x004F) continue;
        const volatile u8 *mi = (const volatile u8 *)0x3200;
        u16 attr = *(volatile const u16 *)mi;
        u16 w = *(volatile const u16 *)(mi + 0x12), h = *(volatile const u16 *)(mi + 0x14);
        u8 bpp = mi[0x19], model = mi[0x1B];
        u32 fb = *(volatile const u32 *)(mi + 0x28);
        if ((attr & 0x91) != 0x91 || model != 6) continue;      /* soportado, grafico, LFB, color directo */
        if ((bpp != 32 && bpp != 16) || !fb || w < 640 || h < 400) continue;
        int dup = -1;
        for (int j = 0; j < nvm; j++) if (vm[j].w == w && vm[j].h == h) dup = j;
        if (dup < 0) { if (nvm >= 48) continue; dup = nvm++; }
        else if (bpp <= vm[dup].bpp) continue;                   /* ya hay uno igual o mejor */
        struct vmode *m = &vm[dup];
        m->mode = mode; m->w = w; m->h = h; m->bpp = bpp;
        m->rs = mi[0x1F]; m->rp = mi[0x20]; m->gs = mi[0x21]; m->gp = mi[0x22];
        m->bs = mi[0x23]; m->bp = mi[0x24];
        m->fb = fb; m->pitch = *(volatile const u16 *)(mi + 0x10);
    }
    for (int i = 1; i < nvm; i++) {                              /* de mayor a menor */
        struct vmode t = vm[i];
        int j = i - 1;
        while (j >= 0 && (u32)vm[j].w * vm[j].h < (u32)t.w * t.h) { vm[j + 1] = vm[j]; j--; }
        vm[j + 1] = t;
    }
    sputs("vbe: "); sdec((u32)nvm); sputs(" modos utiles\n");
    for (int i = 0; i < nvm; i++) {
        sputs("  "); sdec(vm[i].w); sputs("x"); sdec(vm[i].h); sputs("x"); sdec(vm[i].bpp);
        sputs(" modo "); shex(vm[i].mode); sputs(" fb "); shex(vm[i].fb); sputs("\n");
    }
}

static int vbe_set(int i)
{
    struct vmode *m = &vm[i];
    rm_call(0x10, 0x4F02, (u32)m->mode | 0x4000, 0, 0, 0);
    if ((RMR->eax & 0xFFFF) != 0x004F) { sputs("vbe: fallo al poner modo "); shex(m->mode); sputs("\n"); return 0; }
    G.fb = (u8 *)m->fb; G.w = m->w; G.h = m->h; G.pitch = m->pitch; G.bytes = m->bpp / 8; G.m = m;
    cur_vm = i;
    vbe_on = 1;
    return 1;
}

static u32 packc(u32 r, u32 g, u32 b)
{
    struct vmode *m = G.m;
    return ((r >> (8 - m->rs)) << m->rp) | ((g >> (8 - m->gs)) << m->gp) | ((b >> (8 - m->bs)) << m->bp);
}

static void fill_rect(u32 x, u32 y, u32 w, u32 h, u32 col)
{
    if (x >= G.w || y >= G.h || !w || !h) return;
    if (x + w > G.w) w = G.w - x;
    if (y + h > G.h) h = G.h - y;
    for (u32 r = 0; r < h; r++) {
        void *d = G.fb + (y + r) * G.pitch + x * G.bytes;
        u32 n = w;
        if (G.bytes == 4) __asm__ volatile("cld; rep stosl" : "+D"(d), "+c"(n) : "a"(col) : "memory");
        else              __asm__ volatile("cld; rep stosw" : "+D"(d), "+c"(n) : "a"(col) : "memory");
    }
}

static void gtext(u32 x, u32 y, const char *s, u32 sc, u32 col)
{
    for (; *s; s++, x += 8 * sc) {
        const u8 *g = FONT + (u32)(u8)*s * 8;
        for (u32 r = 0; r < 8; r++)
            for (u32 c = 0; c < 8; c++)
                if (g[r] & (0x80 >> c))
                    fill_rect(x + c * sc, y + r * sc, sc, sc, col);
    }
}

static char *audio_bar_text(char *o);

static void draw_bar(void)
{
    char b[110], *o = b;
    u32 green = packc(0x30, 0xFF, 0x30);
    fill_rect(0, 0, G.w, bar_h, 0);
    o = utoa(o, G.w); *o++ = 'x'; o = utoa(o, G.h); *o++ = 'x'; o = utoa(o, G.m->bpp);
    o = scat(o, "  ["); o = utoa(o, (u32)cur_vm + 1); *o++ = '/'; o = utoa(o, (u32)nvm);
    o = scat(o, "]  Ctrl+P: cambiar resolucion");
    *o = 0;
    gtext(8, 2 * fscale, b, fscale, green);
    o = b;
    o = audio_bar_text(o);
    *o = 0;
    gtext(8, 11 * fscale, b, fscale, green);
}

static void gfx_layout(void)
{
    fscale = G.h >= 1400 ? 3 : G.h >= 700 ? 2 : 1;
    bar_h = 21 * fscale;
    u32 avail = G.h - bar_h;
    u32 an = H.aspect_n ? H.aspect_n : 4, ad = H.aspect_d ? H.aspect_d : 3;
    dw = G.w; dh = dw * ad / an;
    if (dh > avail) { dh = avail; dw = dh * an / ad; }
    vx = (G.w - dw) / 2;
    vy = bar_h + (avail - dh) / 2;
    for (u32 i = 0; i <= H.w; i++) xs[i] = (u16)(i * dw / H.w);
    for (u32 j = 0; j <= H.h; j++) ys[j] = (u16)(j * dh / H.h);
    u32 n = 1u << H.bpp;
    for (u32 i = 0; i < n; i++) { u32 g = i * 255 / (n - 1); lvl_pal[i] = packc(g, g, g); }
    fill_rect(0, 0, G.w, G.h, 0);
    draw_bar();
}

static inline u32 pixel(const u8 *b, u32 idx, u32 bpp)
{
    u32 bit = idx * bpp;
    return (b[bit >> 3] >> (8 - bpp - (bit & 7))) & ((1u << bpp) - 1);
}

/* pinta solo lo que cambio respecto a `prev` (full = todo); junta tramos del mismo gris */
static void render_lfb(const u8 *src, const u8 *prev, int full)
{
    u32 bpp = H.bpp;
    for (u32 sy = 0; sy < H.h; sy++) {
        u32 y0 = vy + ys[sy], y1 = vy + ys[sy + 1];
        if (y1 == y0) continue;
        u32 base = sy * H.w, sx = 0;
        while (sx < H.w) {
            u32 v = pixel(src, base + sx, bpp);
            if (!full && pixel(prev, base + sx, bpp) == v) { sx++; continue; }
            u32 ex = sx + 1;
            while (ex < H.w && pixel(src, base + ex, bpp) == v &&
                   (full || pixel(prev, base + ex, bpp) != v)) ex++;
            fill_rect(vx + xs[sx], y0, (u32)xs[ex] - xs[sx], y1 - y0, lvl_pal[v]);
            sx = ex;
        }
    }
}

/* Ctrl+P (1) y Ctrl+O (2) por el controlador PS/2 (por polling) */
static int kbd_poll(void)
{
    static u8 ctrl;
    int ev = 0;
    for (int n = 0; n < 16; n++) {
        u8 st = inb(0x64);
        if (!(st & 1)) break;
        u8 sc = inb(0x60);
        if (st & 0x20) continue;                 /* dato del raton */
        if (sc == 0x1D) ctrl = 1;
        else if (sc == 0x9D) ctrl = 0;
        else if (sc == 0x19 && ctrl) ev |= 1;
        else if (sc == 0x18 && ctrl) ev |= 2;
    }
    return ev;
}

static void screen_msg(const char *a, const char *b, int err)
{
    if (vbe_on) {
        fill_rect(0, 0, G.w, G.h, 0);
        gtext(16, 16, a, 2, err ? packc(255, 70, 70) : packc(255, 255, 255));
        gtext(16, 48, b, 2, packc(255, 255, 255));
    } else {
        cls();
        text(8, 8, a, err ? COL_RED : COL_WHITE);
        text(8, 24, b, COL_WHITE);
    }
}

static void fatal(const char *msg)
{
    sputs("FATAL: "); sputs(msg); sputs("\n");
    screen_msg("BADKERNEL: ERROR", msg, 1);
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

/* salida de audio elegida (0 = todas) */
static int out_sel, out_n;

static int snd_out_count(void)
{
    return snd == 1 ? hda_out_count() : snd == 2 ? ac97_out_count() : 0;
}

static const char *snd_out_name(int i)
{
    return snd == 1 ? hda_out_name(i) : ac97_out_name(i);
}

static void snd_out_select(int i)
{
    if (snd == 1) hda_out_select(i); else ac97_out_select(i);
}

static char *audio_bar_text(char *o)
{
    if (!snd) return scat(o, "Sin audio");
    o = scat(o, snd == 1 ? "Audio HDA: " : "Audio AC'97: ");
    o = scat(o, snd_out_name(out_sel));
    if (out_n > 1) {
        o = scat(o, "  ["); o = utoa(o, (u32)out_sel + 1); *o++ = '/'; o = utoa(o, (u32)out_n);
        o = scat(o, "]  Ctrl+O: cambiar salida");
    }
    return o;
}

static void next_output(void)
{
    if (out_n < 2) { sputs("audio: solo hay una salida\n"); return; }
    out_sel = (out_sel + 1) % out_n;
    snd_out_select(out_sel);
    sputs("audio: salida "); sputs(snd_out_name(out_sel)); sputs("\n");
    if (vbe_on) draw_bar();
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
    char m[] = "EXC 00 EIP 00000000";
    m[4] = (char)('0' + v / 10); m[5] = (char)('0' + v % 10);
    for (int i = 0; i < 8; i++) m[11 + i] = "0123456789ABCDEF"[(eip >> (28 - 4 * i)) & 15];
    screen_msg("BADKERNEL: EXCEPCION", m, 1);
}

/* ---------------------------------------------------------------- kmain */

static int have_prev;

static void show_frame(u32 f)
{
    if (ata_read(H.video_lba + f * H.frame_sectors, H.frame_sectors, FRAMEBUF) < 0)
        fatal("error de lectura de video");
    if (vbe_on) {
        render_lfb(FRAMEBUF, PREV, !have_prev);
        memcpy(PREV, FRAMEBUF, H.frame_sectors * 512);
        have_prev = 1;
    } else {
        blit(FRAMEBUF, H.w, H.bpp);
    }
}

/* Ctrl+P: siguiente resolucion de la lista (de mayor a menor, y vuelta al mayor) */
static void next_mode(void)
{
    for (int k = 1; k <= nvm; k++) {
        int i = (cur_vm + k) % nvm;
        if (vbe_set(i)) {
            gfx_layout();
            render_lfb(PREV, PREV, 1);
            sputs("modo "); sdec(G.w); sputs("x"); sdec(G.h); sputs("\n");
            return;
        }
    }
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
    sputs("\nBadKernel 0.4\n");
    idt_init();
    pit_init();
    palette_init(4);
    cls();
    text(8, 8, "BADKERNEL 0.4", COL_WHITE);
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
        out_n = snd_out_count();
        for (int i = 0; i < out_n; i++) { sputs("  salida "); sdec((u32)i + 1); sputs(": "); sputs(snd_out_name(i)); sputs("\n"); }
    } else if (H.audio_bytes) {
        sputs("audio no es s16 mono a 48000 Hz (o fps no divide 48000): se ignora\n");
    }

    cls();
    text(8, 8, "BADKERNEL 0.4", COL_WHITE);
    text(8, 24, snd == 1 ? "audio: Intel HDA" : snd == 2 ? "audio: AC'97" : "sin audio", COL_GREEN);
    delay_ms(1200);
    cls();

    gdt_init();
    vbe_enum();
    for (int i = 0; i < nvm && !vbe_on; i++)
        if (vbe_set(i)) gfx_layout();
    if (vbe_on) { sputs("video: "); sdec(G.w); sputs("x"); sdec(G.h); sputs("\n"); }
    else sputs("video: sigo en modo 13h (320x200)\n");

    if (snd) audio_start();
    pit_t0 = pit_now();

    s32 last = -1;
    u32 shown = 0, skipped = 0;
    for (;;) {
        if (snd) audio_poll();
        int ev = kbd_poll();
        if ((ev & 1) && vbe_on) next_mode();
        if ((ev & 2) && snd) next_output();
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
    screen_msg("FIN", "gracias por ver Bad Apple!!", 0);
    for (;;) { __asm__ volatile("cli; hlt"); }
}
