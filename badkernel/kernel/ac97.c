/* ac97.c - audio AC'97 (Intel ICH y compatibles: VIA, SiS, nForce, AMD...)
 *
 * Los registros del codec (mezclador, NAM = BAR0) y el controlador de bus maestro (NABM = BAR1)
 * estan en puertos de E/S. Se reproduce PCM estereo de 16 bits a 48 kHz (la frecuencia por
 * defecto que todo codec AC'97 soporta, sin VRA) con una lista de 32 descriptores que
 * recorre el anillo del motor de audio (audio.c). Sin interrupciones: se actualiza LVI desde
 * ac97_pos(), que el motor llama continuamente.
 */
#include "kernel.h"

/* mezclador (NAM) */
#define NAM_RESET   0x00
#define NAM_MASTER  0x02
#define NAM_HEADPH  0x04
#define NAM_MONO    0x06
#define NAM_PCM     0x18
#define NAM_EXTCTL  0x2A
#define NAM_VENDOR1 0x7C
#define NAM_VENDOR2 0x7E
/* bus maestro (NABM), canal PCM salida */
#define PO_BDBAR    0x10
#define PO_CIV      0x14
#define PO_LVI      0x15
#define PO_SR       0x16
#define PO_PICB     0x18
#define PO_CR       0x1B
#define GLOB_CNT    0x2C
#define GLOB_STA    0x30

#define NENT 32

static u16 nam, nabm;
static int state;                       /* 0 sin probar, 1 listo, -1 no hay */
static u32 ring_bytes;
static u32 ent_bytes;
static u32 bdl[NENT * 2] __attribute__((aligned(8)));
static char info[64] = "AUDIO: NO PROBADO";

static void nw(int r, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"((u16)(nam + r))); }
static u16 nr(int r) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"((u16)(nam + r))); return v; }
static void bw8(int r, u8 v) { outb((u16)(nabm + r), v); }
static u8 br8(int r) { return inb((u16)(nabm + r)); }
static void bw16(int r, u16 v) { __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"((u16)(nabm + r))); }
static u16 br16(int r) { u16 v; __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"((u16)(nabm + r))); return v; }
static void bw32(int r, u32 v) { __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"((u16)(nabm + r))); }
static u32 br32(int r) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"((u16)(nabm + r))); return v; }

static int ac97_init(void)
{
    int bus, dev, fn, i;
    u32 b0, b1, cmd;

    if (!pci_find(0x04, 0x01, -1, 0, &bus, &dev, &fn))
        return 0;
    b0 = pci_read32(bus, dev, fn, 0x10);
    b1 = pci_read32(bus, dev, fn, 0x14);
    if (!(b0 & 1) || !(b1 & 1))
        return 0;                               /* solo registros en puertos de E/S */
    nam = (u16)(b0 & 0xFFFC);
    nabm = (u16)(b1 & 0xFFFC);
    cmd = pci_read32(bus, dev, fn, 0x04) & 0xFFFF;
    pci_write32(bus, dev, fn, 0x04, cmd | 0x5);   /* E/S + bus maestro */

    br32(GLOB_CNT);
    bw32(GLOB_CNT, 0x02);                       /* sacar el enlace AC del reset en frio */
    for (i = 0; i < 200 && !(br32(GLOB_STA) & 0x100); i++)   /* codec primario listo */
        delay_ms(1);
    if (!(br32(GLOB_STA) & 0x100)) {
        bw32(GLOB_CNT, 0x03);                   /* reintento con GIE */
        for (i = 0; i < 300 && !(br32(GLOB_STA) & 0x100); i++)
            delay_ms(1);
        if (!(br32(GLOB_STA) & 0x100))
            return 0;
    }
    nw(NAM_RESET, 0);
    for (i = 0; i < 100 && (nr(0x26) & 0xF) != 0xF; i++)    /* analogico listo */
        delay_ms(1);
    nw(NAM_MASTER, 0x0000);                     /* 0 dB, sin silencio */
    nw(NAM_HEADPH, 0x0000);
    nw(NAM_MONO, 0x0000);
    nw(NAM_PCM, 0x0808);                        /* PCM un poco bajo para no saturar */
    if (nr(NAM_EXTCTL) & 1)
        nw(NAM_EXTCTL, nr(NAM_EXTCTL) & ~1u);   /* VRA apagado: 48 kHz fijo */
    return 1;
}

int ac97_open(void)
{
    if (state == 0) {
        state = ac97_init() ? 1 : -1;
        if (state > 0) {
            const char *s = "AUDIO: AC'97 CODEC ";
            u16 v1 = nr(NAM_VENDOR1), v2 = nr(NAM_VENDOR2);
            char *o = info;
            int i;
            while (*s)
                *o++ = *s++;
            for (i = 12; i >= 0; i -= 4)
                *o++ = "0123456789ABCDEF"[(v1 >> i) & 15];
            for (i = 12; i >= 0; i -= 4)
                *o++ = "0123456789ABCDEF"[(v2 >> i) & 15];
            *o = 0;
        }
    }
    return state > 0;
}

const char *ac97_info(void)
{
    return info;
}

void ac97_start(void *ring, u32 bytes)
{
    int i;
    ring_bytes = bytes;
    ent_bytes = bytes / NENT;
    bw8(PO_CR, 0);
    bw8(PO_CR, 0x02);                           /* reset del canal */
    for (i = 0; i < 100 && (br8(PO_CR) & 0x02); i++)
        delay_ms(1);
    for (i = 0; i < NENT; i++) {
        bdl[i * 2] = (u32)(unsigned long)ring + (u32)i * ent_bytes;
        bdl[i * 2 + 1] = (ent_bytes / 2) & 0xFFFF;   /* longitud en muestras de 16 bits */
    }
    bw32(PO_BDBAR, (u32)(unsigned long)bdl);
    bw8(PO_LVI, NENT - 1);
    bw16(PO_SR, 0x1C);                          /* limpiar estado */
    bw8(PO_CR, 0x01);                           /* RPBM: correr */
}

u32 ac97_pos(void)
{
    u32 civ = br8(PO_CIV) & (NENT - 1);
    u32 picb = br16(PO_PICB);                   /* muestras que faltan de la entrada actual */
    u32 done;
    bw8(PO_LVI, (u8)((civ + NENT - 1) & (NENT - 1)));   /* deja correr toda la vuelta */
    if (br16(PO_SR) & 0x01)                     /* se detuvo (anillo agotado): reanudar */
        bw8(PO_CR, 0x01);
    done = ent_bytes - picb * 2;
    if (done > ent_bytes)
        done = 0;
    return (civ * ent_bytes + done) % ring_bytes;
}

void ac97_stop(void)
{
    bw8(PO_CR, 0);
}
