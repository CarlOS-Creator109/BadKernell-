; BadKernel - punto de entrada y stubs de excepciones
bits 32

extern kmain
extern exc_handler
extern __bss_start
extern __bss_end

section .text.start
global _start
_start:
    cld
    mov edi, __bss_start
    mov ecx, __bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb
    call kmain
.hang:
    cli
    hlt
    jmp .hang

section .text
exc_common:
    mov eax, esp
    push eax
    call exc_handler
.h:
    cli
    hlt
    jmp .h

%macro STUB 1
section .text
exc_stub_%1:
    push dword %1
    jmp exc_common
section .data
    dd exc_stub_%1
%endmacro

section .data
align 4
global exc_stub_table
exc_stub_table:
%assign i 0
%rep 32
    STUB i
%assign i i+1
%endrep

; ---------------------------------------------------------------------------
; bios_int(n): ejecuta INT n en modo real. Registros en la estructura de 0x7000:
;   eax 0, ebx 4, ecx 8, edx 12, esi 16, edi 20, es 24, ds 28, eflags 32
; Necesita GDT con 0x18 = codigo 16 bits y 0x20 = datos 16 bits (base 0, limite 64K),
; y que este codigo viva por debajo de 64 KiB (seccion .text.thunk, al inicio).
; ---------------------------------------------------------------------------
section .text.thunk progbits alloc exec write align=16
global bios_int
bits 32
bios_int:
    push ebp
    mov ebp, esp
    pushad
    mov al, [ebp + 8]
    mov [int_op + 1], al
    mov [saved_esp], esp
    sidt [saved_idt]
    lidt [rm_idtr]
    jmp 0x18:pm16

bits 16
pm16:
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov sp, 0x7000
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax
    jmp 0x0000:rm16

rm16:
    xor ax, ax
    mov ds, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov sp, 0x7000
    mov es, [0x7018]
    mov ebx, [0x7004]
    mov ecx, [0x7008]
    mov edx, [0x700C]
    mov esi, [0x7010]
    mov edi, [0x7014]
    mov eax, [0x7000]
int_op:
    db 0xCD, 0x10
    cli
    mov [0x7000], eax
    mov [0x7004], ebx
    mov [0x7008], ecx
    mov [0x700C], edx
    mov [0x7010], esi
    mov [0x7014], edi
    pushfd
    pop eax
    mov [0x7020], eax
    mov eax, cr0
    or  eax, 1
    mov cr0, eax
    jmp 0x08:pm32b

bits 32
pm32b:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov esp, [saved_esp]
    lidt [saved_idt]
    popad
    pop ebp
    ret

saved_esp: dd 0
saved_idt: times 6 db 0
rm_idtr:   dw 0x03FF
           dd 0
