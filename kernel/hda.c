/* hda.c - audio Intel High Definition Audio (la tarjeta de sonido de casi toda PC desde 2004)
 *
 * Por sondeo, sin interrupciones. Se inicializa solo la primera vez que se usa (BEEP):
 *   controlador: reset, CORB/RIRB (cola de comandos al codec)
 *   codec: busca los pines de salida (altavoz, auriculares, line-out), el DAC al que llegan,
 *          los habilita y quita el silencio de cada widget del camino
 *   flujo: un buffer circular con un tono (onda seno) a 48 kHz, 16 bits, estereo
 * Si algo falla, BEEP vuelve al altavoz del PC.
 */
#include "kernel.h"

/* registros del controlador */
#define GCAP      0x00
#define GCTL      0x08
#define STATESTS  0x0E
#define CORBLBASE 0x40
#define CORBUBASE 0x44
#define CORBWP    0x48
#define CORBRP    0x4A
#define CORBCTL   0x4C
#define CORBSIZE  0x4E
#define RIRBLBASE 0x50
#define RIRBUBASE 0x54
#define RIRBWP    0x58
#define RINTCNT   0x5A
#define RIRBCTL   0x5C
#define RIRBSIZE  0x5E
/* descriptor de flujo (desplazamiento desde su base) */
#define SD_CTL    0x00
#define SD_CBL    0x08
#define SD_LVI    0x0C
#define SD_FMT    0x12
#define SD_BDLPL  0x18
#define SD_BDLPU  0x1C

#define RATE      48000
#define STREAM_TAG 1

static volatile u8 *regs;
static int state;                       /* 0 sin probar, 1 listo, -1 no disponible */
static int sd_off;                      /* base del primer flujo de salida */
static int rirb_rp;
static int corb_wp;
static u32 corb[256] __attribute__((aligned(128)));
static u32 rirb[512] __attribute__((aligned(128)));        /* 256 entradas de 8 bytes */
static u32 bdl[16] __attribute__((aligned(128)));          /* 2 entradas de 16 bytes */

static char info[120] = "AUDIO: NO PROBADO";
static int cad_used = -1;
static int out_pin[8];
static u32 out_cfg[8];
static int out_cnt;

static u8  r8(int o)  { return *(volatile u8 *)(regs + o); }
static u16 r16(int o) { return *(volatile u16 *)(regs + o); }
static u32 r32(int o) { return *(volatile u32 *)(regs + o); }
static void w8(int o, u8 v)   { *(volatile u8 *)(regs + o) = v; }
static void w16(int o, u16 v) { *(volatile u16 *)(regs + o) = v; }
static void w32(int o, u32 v) { *(volatile u32 *)(regs + o) = v; }

/* ---- comandos al codec ---- */

static int hda_cmd(int cad, int nid, u32 verb, u32 *resp)
{
    int i, idx;
    u32 cmd = ((u32)cad << 28) | ((u32)nid << 20) | verb;

    corb_wp = (corb_wp + 1) & 255;
    corb[corb_wp] = cmd;
    w16(CORBWP, (u16)corb_wp);
    for (i = 0; i < 200; i++) {                 /* hasta 200 ms */
        while ((r16(RIRBWP) & 255) != rirb_rp) {
            rirb_rp = (rirb_rp + 1) & 255;
            w8(0x5D, 0x05);                     /* limpiar RIRBSTS (respuesta/overrun) */
            idx = rirb_rp * 2;
            if (rirb[idx + 1] & 0x10)           /* respuesta no solicitada: ignorar */
                continue;
            if (resp)
                *resp = rirb[idx];
            return 1;
        }
        delay_ms(1);
    }
    return 0;
}

static u32 get(int cad, int nid, u32 verb12, u32 payload)
{
    u32 r = 0;
    hda_cmd(cad, nid, (verb12 << 8) | payload, &r);
    return r;
}

static void set(int cad, int nid, u32 verb12, u32 payload)
{
    hda_cmd(cad, nid, (verb12 << 8) | payload, NULL);
}

static void set4(int cad, int nid, u32 verb4, u32 payload16)
{
    hda_cmd(cad, nid, (verb4 << 16) | payload16, NULL);
}

/* ---- topologia del codec ---- */

#define W_DAC 0
#define W_MIXER 2
#define W_SEL 3
#define W_PIN 4

static u32 wcaps[256];
static int afg_start, afg_count, afg_nid;
static u32 afg_outamp;

static int wtype(int nid) { return (int)((wcaps[nid] >> 20) & 15); }

/* Entradas de la lista de conexiones del widget: devuelve cuantas */
static int conns(int cad, int nid, int *list, int max)
{
    u32 len = get(cad, nid, 0xF00, 0x0E);
    int n = (int)(len & 0x7F), long_form = (len & 0x80) != 0, i, k;

    if (n > max)
        n = max;
    for (i = 0; i < n;) {
        u32 r = get(cad, nid, 0xF02, (u32)i);
        int per = long_form ? 2 : 4;
        for (k = 0; k < per && i < n; k++, i++)
            list[i] = long_form ? (int)((r >> (16 * k)) & 0xFFFF) : (int)((r >> (8 * k)) & 0xFF);
    }
    return n;
}

static int path_nid[8], path_idx[8], path_len;

/* Busca (en profundidad) un camino desde nid hasta un DAC. Rellena path_* de DAC hacia nid. */
static int find_dac(int cad, int nid, int depth)
{
    int list[16], n, i;

    if (wtype(nid) == W_DAC) {
        path_len = 0;
        path_nid[path_len] = nid;
        path_idx[path_len++] = 0;
        return 1;
    }
    if (depth == 0)
        return 0;
    n = conns(cad, nid, list, 16);
    for (i = 0; i < n; i++) {
        if (list[i] < afg_start || list[i] >= afg_start + afg_count)
            continue;
        if (find_dac(cad, list[i], depth - 1)) {
            if (path_len < 8) {
                path_nid[path_len] = nid;
                path_idx[path_len++] = i;       /* indice de conexion usado en este widget */
            }
            return 1;
        }
    }
    return 0;
}

static void unmute_widget(int cad, int nid, int idx)
{
    u32 caps = wcaps[nid];
    u32 amp = (caps & 8) ? get(cad, nid, 0xF00, 0x12) : afg_outamp;
    u32 gain = amp & 0x7F;                      /* offset = 0 dB */

    set(cad, nid, 0x705, 0);                    /* D0 */
    if (caps & 4)                               /* tiene amplificador de salida */
        set4(cad, nid, 0x3, 0xB000 | gain);
    if (caps & 2)                               /* tiene amplificador de entrada */
        set4(cad, nid, 0x3, 0x7000 | ((u32)idx << 8) | gain);
}

static int setup_path(int cad, int pin)
{
    int i;
    u32 cfg = get(cad, pin, 0xF1C, 0), pcaps;

    if (!find_dac(cad, pin, 4))
        return 0;
    /* path_*: [0] = DAC ... [path_len-1] = pin */
    for (i = path_len - 1; i >= 0; i--) {
        int nid = path_nid[i];
        if (wtype(nid) == W_DAC) {
            set(cad, nid, 0x705, 0);
            if (wcaps[nid] & 4)
                set4(cad, nid, 0x3, 0xB000 | ((wcaps[nid] & 8 ? get(cad, nid, 0xF00, 0x12) : afg_outamp) & 0x7F));
            set(cad, nid, 0x706, (STREAM_TAG << 4) | 0);
            set4(cad, nid, 0x2, 0x0011);        /* 48 kHz, 16 bits, 2 canales */
        } else {
            unmute_widget(cad, nid, path_idx[i]);
            if (wtype(nid) != W_MIXER)
                set(cad, nid, 0x701, (u32)path_idx[i]);   /* conexion elegida */
        }
    }
    pcaps = get(cad, pin, 0xF00, 0x0C);
    set(cad, pin, 0x707, ((cfg >> 20) & 15) == 2 ? 0xC0 : 0x40);   /* salida (+ auriculares) */
    if (pcaps & (1u << 16))
        set(cad, pin, 0x70C, 2);                /* EAPD: enciende el amplificador externo */
    return 1;
}

static int codec_init(int cad)
{
    u32 r, vid;
    int root_n, root_start, i, nid, pins = 0;

    vid = get(cad, 0, 0xF00, 0x00);
    r = get(cad, 0, 0xF00, 0x04);
    root_start = (int)((r >> 16) & 0xFF);
    root_n = (int)(r & 0xFF);
    afg_nid = 0;
    for (i = 0; i < root_n; i++)                /* grupo de funcion de audio */
        if ((get(cad, root_start + i, 0xF00, 0x05) & 0xFF) == 1) {
            afg_nid = root_start + i;
            break;
        }
    if (!afg_nid)
        return 0;
    set(cad, afg_nid, 0x705, 0);                /* D0 */
    delay_ms(10);
    r = get(cad, afg_nid, 0xF00, 0x04);
    afg_start = (int)((r >> 16) & 0xFF);
    afg_count = (int)(r & 0xFF);
    if (afg_start + afg_count > 255)
        afg_count = 255 - afg_start;
    afg_outamp = get(cad, afg_nid, 0xF00, 0x12);
    for (nid = afg_start; nid < afg_start + afg_count; nid++)
        wcaps[nid] = get(cad, nid, 0xF00, 0x09);

    for (nid = afg_start; nid < afg_start + afg_count && pins < 6; nid++) {
        u32 cfg, dev;
        if (wtype(nid) != W_PIN)
            continue;
        cfg = get(cad, nid, 0xF1C, 0);
        dev = (cfg >> 20) & 15;
        if (((cfg >> 30) & 3) == 1 || dev > 2)  /* sin conexion fisica / no es salida */
            continue;
        if (!(get(cad, nid, 0xF00, 0x0C) & (1u << 4)))
            continue;                           /* el pin no puede ser salida */
        if (setup_path(cad, nid)) {
            out_pin[out_cnt] = nid;
            out_cfg[out_cnt] = cfg;
            out_cnt++;
            pins++;
        }
    }
    {
        char *o = info;
        const char *s = "AUDIO: HDA CODEC ";
        while (*s)
            *o++ = *s++;
        for (i = 7; i >= 0; i--)
            *o++ = "0123456789ABCDEF"[(vid >> (4 * i)) & 15];
        s = ", PINS ";
        while (*s)
            *o++ = *s++;
        *o++ = (char)('0' + pins);
        *o = 0;
    }
    return pins;
}

/* ---- controlador ---- */

static int hda_init(void)
{
    int bus, dev, fn, i, cad;
    u32 bar, gcap;
    u16 sts;

    if (!pci_find(0x04, 0x03, -1, 0, &bus, &dev, &fn))
        return 0;
    bar = pci_read32(bus, dev, fn, 0x10);
    if (bar & 1)
        return 0;
    pci_write32(bus, dev, fn, 0x04, (pci_read32(bus, dev, fn, 0x04) & 0xFFFF) | 0x6);
    regs = (volatile u8 *)(bar & ~0xFu);
    if (!regs)
        return 0;

    w32(GCTL, r32(GCTL) & ~1u);                 /* reset del controlador */
    for (i = 0; i < 100 && (r32(GCTL) & 1); i++)
        delay_ms(1);
    delay_ms(2);
    w32(GCTL, r32(GCTL) | 1u);
    for (i = 0; i < 100 && !(r32(GCTL) & 1); i++)
        delay_ms(1);
    if (!(r32(GCTL) & 1))
        return 0;
    delay_ms(10);                               /* los codecs tardan en anunciarse */
    sts = r16(STATESTS);
    if (!(sts & 0x7FFF))
        return 0;

    gcap = r16(GCAP);
    if (!((gcap >> 12) & 15))
        return 0;                               /* sin flujos de salida */
    sd_off = 0x80 + (int)((gcap >> 8) & 15) * 0x20;

    w8(CORBCTL, 0);                             /* CORB: 256 entradas */
    w8(RIRBCTL, 0);
    delay_ms(1);
    if (r8(CORBSIZE) & 0x40)
        w8(CORBSIZE, 2);
    if (r8(RIRBSIZE) & 0x40)
        w8(RIRBSIZE, 2);
    w32(CORBLBASE, (u32)(unsigned long)corb);
    w32(CORBUBASE, 0);
    w32(RIRBLBASE, (u32)(unsigned long)rirb);
    w32(RIRBUBASE, 0);
    w16(CORBRP, 0x8000);                        /* reiniciar punteros */
    for (i = 0; i < 100 && !(r16(CORBRP) & 0x8000); i++)
        delay_ms(1);
    w16(CORBRP, 0);
    for (i = 0; i < 100 && (r16(CORBRP) & 0x8000); i++)
        delay_ms(1);
    w16(RIRBWP, 0x8000);
    w16(RINTCNT, 0xFF);
    corb_wp = 0;
    rirb_rp = 0;
    w16(CORBWP, 0);
    w8(CORBCTL, 2);                             /* CORB DMA */
    w8(RIRBCTL, 2);                             /* RIRB DMA */
    delay_ms(2);

    for (cad = 0; cad < 15; cad++) {
        if (!(sts & (1u << cad)))
            continue;
        if (codec_init(cad) > 0) {
            cad_used = cad;
            return 1;
        }
    }
    return 0;
}

/* ---- salida por DMA: el motor de audio (audio.c) maneja un anillo ciclico ---- */

static int is_open;

int hda_open(void)
{
    if (state == 0)
        state = hda_init() ? 1 : -1;
    is_open = state > 0;
    return is_open;
}

/* anillo ciclico de `bytes` (multiplo de 2 KB): 2 entradas BDL de media vuelta cada una */
void hda_start(void *ring, u32 bytes)
{
    int k, ctl;

    w32(sd_off + SD_CTL, r32(sd_off + SD_CTL) | 1u);            /* reset */
    for (k = 0; k < 100 && !(r32(sd_off + SD_CTL) & 1); k++)
        delay_ms(1);
    w32(sd_off + SD_CTL, r32(sd_off + SD_CTL) & ~1u);
    for (k = 0; k < 100 && (r32(sd_off + SD_CTL) & 1); k++)
        delay_ms(1);
    bdl[0] = (u32)(unsigned long)ring;               bdl[1] = 0; bdl[2] = bytes / 2; bdl[3] = 0;
    bdl[4] = (u32)(unsigned long)ring + bytes / 2;   bdl[5] = 0; bdl[6] = bytes / 2; bdl[7] = 0;
    w32(sd_off + SD_BDLPL, (u32)(unsigned long)bdl);
    w32(sd_off + SD_BDLPU, 0);
    w32(sd_off + SD_CBL, bytes);
    w16(sd_off + SD_LVI, 1);
    w16(sd_off + SD_FMT, 0x0011);                    /* 48 kHz, 16 bits, estereo */
    ctl = (int)(r32(sd_off + SD_CTL) & 0xFF0000FF);
    w32(sd_off + SD_CTL, (u32)ctl | ((u32)STREAM_TAG << 20));
    w8(sd_off + 0x03, 0x1C);                         /* limpiar estado */
    w32(sd_off + SD_CTL, r32(sd_off + SD_CTL) | 2u); /* RUN */
}

u32 hda_pos(void)
{
    return r32(sd_off + 0x04);                       /* LPIB: bytes reproducidos en la vuelta */
}

void hda_stop(void)
{
    w32(sd_off + SD_CTL, r32(sd_off + SD_CTL) & ~2u);
}

const char *hda_info(void)
{
    return info;
}

/* ---- seleccion de salida: 0 = todas, 1..n = cada pin de salida por separado ---- */

int hda_out_count(void)
{
    return out_cnt > 1 ? out_cnt + 1 : 1;
}

const char *hda_out_name(int i)
{
    if (i <= 0 || i > out_cnt)
        return "Todas";
    switch ((out_cfg[i - 1] >> 20) & 15) {
    case 0:  return "Linea";
    case 1:  return "Bocinas";
    case 2:  return "Audifonos";
    default: return "Salida";
    }
}

void hda_out_select(int i)
{
    int k;

    if (state <= 0 || out_cnt < 2)
        return;
    for (k = 0; k < out_cnt; k++) {
        if (i >= 1 && k != i - 1) {                     /* apaga los demas pines */
            set(cad_used, out_pin[k], 0x707, 0);
            if (get(cad_used, out_pin[k], 0xF00, 0x0C) & (1u << 16))
                set(cad_used, out_pin[k], 0x70C, 0);    /* EAPD apagado */
        }
    }
    if (i < 1 || i > out_cnt) {
        for (k = 0; k < out_cnt; k++)
            setup_path(cad_used, out_pin[k]);
    } else {
        setup_path(cad_used, out_pin[i - 1]);           /* vuelve a armar la ruta de este */
    }
}
