#ifndef KERNEL_H
#define KERNEL_H
/* BadKernel - definiciones compartidas (los drivers hda.c y ac97.c vienen de PC BASIC) */

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef signed int     s32;
#define NULL ((void *)0)

static inline void outb(u16 p, u8 v)  { __asm__ volatile("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline u8   inb(u16 p)         { u8 v;  __asm__ volatile("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outw(u16 p, u16 v) { __asm__ volatile("outw %0,%1" :: "a"(v), "Nd"(p)); }
static inline u16  inw(u16 p)         { u16 v; __asm__ volatile("inw %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0,%1" :: "a"(v), "Nd"(p)); }
static inline u32  inl(u16 p)         { u32 v; __asm__ volatile("inl %1,%0" : "=a"(v) : "Nd"(p)); return v; }

/* kernel.c */
void sputs(const char *s);
void sdec(u32 v);
void shex(u32 v);
void delay_ms(int ms);
u32  pci_read32(int bus, int dev, int fn, int off);
void pci_write32(int bus, int dev, int fn, int off, u32 v);
int  pci_find(int cls, int sub, int prog, int nth, int *bus, int *dev, int *fn);

/* hda.c / ac97.c: anillo ciclico de PCM s16 estereo a 48 kHz */
int  hda_open(void);
void hda_start(void *ring, u32 bytes);
u32  hda_pos(void);
void hda_stop(void);
const char *hda_info(void);

int  ac97_open(void);
void ac97_start(void *ring, u32 bytes);
u32  ac97_pos(void);
void ac97_stop(void);
const char *ac97_info(void);

#endif
