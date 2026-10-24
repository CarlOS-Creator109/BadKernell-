; BadKernel - stage2 (modo real -> modo protegido)
;  * guarda la fuente ROM 8x8 de la BIOS en 0x70000 (para dibujar texto en modo 13h)
;  * activa A20, pone modo de video 13h (320x200x256)
;  * entra a modo protegido de 32 bits y salta al kernel en 0x8600
; Tamano fijo: 2048 bytes (el kernel va justo despues).
org 0x7E00
bits 16

KERNEL_ADDR equ 0x7E00 + 2048

stage2:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax

    mov ax, 0x1130          ; INT 10h: obtener puntero a fuente
    mov bh, 0x03            ; ROM 8x8 (caracteres 0-127)
    int 0x10                ; -> ES:BP
    push ds
    push es
    pop ds                  ; DS = segmento de la fuente
    mov si, bp
    mov ax, 0x7000
    mov es, ax
    xor di, di
    mov cx, 1024            ; 2048 bytes = 256 chars * 8
    cld
    rep movsw
    pop ds
    xor ax, ax
    mov es, ax

    in  al, 0x92            ; A20 rapido
    or  al, 2
    and al, 0xFE
    out 0x92, al

    mov ax, 0x0013          ; modo grafico 13h
    int 0x10

    cli
    lgdt [gdtr]
    mov eax, cr0
    or  al, 1
    mov cr0, eax
    jmp 0x08:pm32

bits 32
pm32:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, 0x90000
    mov eax, KERNEL_ADDR
    jmp eax

align 8
gdt:
    dq 0
    dq 0x00CF9A000000FFFF   ; 0x08 codigo 32 bits, base 0, 4 GiB
    dq 0x00CF92000000FFFF   ; 0x10 datos
gdtr:
    dw gdtr - gdt - 1
    dd gdt

times 2048-($-$$) db 0
