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
