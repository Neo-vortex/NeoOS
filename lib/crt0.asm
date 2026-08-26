; lib/crt0.asm — C runtime startup. The ELF entry point
; (userland/user.ld's ENTRY(_start)); milestone 5's
; kernel_thread_trampoline `iretq` lands here.
;
; Written in assembly, not C, to avoid GCC generating a stack-frame
; prologue for _start that could disturb the SysV ABI's
; 16-byte-alignment-before-`call` requirement -- nothing has
; established a normal call chain yet at process entry. USER_STACK_TOP
; (milestone 5's 0x0000700000000000) is already 16-byte aligned, and
; _start pushes nothing before `call main`, so alignment holds.

extern main
extern exit

section .text
[bits 64]
global _start

_start:
    xor edi, edi   ; argc = 0
    xor esi, esi   ; argv = NULL
    call main
    mov edi, eax   ; exit(main's return value)
    call exit
.hang:             ; exit() never returns, but halt safely if it somehow does
    hlt
    jmp .hang
