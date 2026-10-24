; BadKernel - sector de arranque (MBR)
; Carga stage2 + kernel (KSECT sectores desde LBA 1) a 0x7E00 con INT 13h extendido
; y salta a stage2.  Compilar con: nasm -f bin -DKSECT=<n> boot.asm
org 0x7C00
bits 16

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00
    sti
    mov [bootdrv], dl

    mov si, msg_boot
    call print

    ; BIOS con extensiones de disco?
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [bootdrv]
    int 0x13
    jc  .noext
    cmp bx, 0xAA55
    jne .noext

    mov word [dap_seg], 0x07E0       ; 0x7E00 fisico
    mov bp, KSECT                    ; sectores restantes

.loop:
    mov cx, bp
    cmp cx, 32
    jbe .c1
    mov cx, 32                       ; bloques de 16 KiB
.c1:
    mov [dap_cnt], cx
    mov si, dap
    mov dl, [bootdrv]
    mov ah, 0x42
    int 0x13
    jc  .err
    mov ax, [dap_cnt]
    add [dap_lba], ax
    shl ax, 5                        ; sectores * 512 / 16 = parrafos
    add [dap_seg], ax
    sub bp, [dap_cnt]
    jnz .loop

    mov dl, [bootdrv]
    jmp 0x0000:0x7E00

.noext:
    mov si, msg_noext
    jmp .die
.err:
    mov si, msg_err
.die:
    call print
.halt:
    hlt
    jmp .halt

print:
    lodsb
    or al, al
    jz .done
    mov ah, 0x0E
    mov bx, 0x0007
    int 0x10
    jmp print
.done:
    ret

bootdrv:   db 0
msg_boot:  db "BadKernel", 13, 10, 0
msg_noext: db "sin INT13 ext", 0
msg_err:   db "error de disco", 0

dap:
    db 16, 0
dap_cnt: dw 0
dap_off: dw 0
dap_seg: dw 0
dap_lba: dd 1, 0

; tabla de particiones ficticia (algunas BIOS la exigen)
times 446-($-$$) db 0
    db 0x80, 0x00, 0x02, 0x00        ; activa, CHS inicio
    db 0xDA, 0xFE, 0xFF, 0xFF        ; tipo, CHS fin
    dd 1                             ; LBA inicio
    dd 0x0001FFFF                    ; sectores
times 510-($-$$) db 0
dw 0xAA55
