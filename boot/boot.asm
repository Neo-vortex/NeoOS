; boot/boot.asm — Multiboot2 header + minimal 32-bit entry point

MULTIBOOT2_MAGIC    equ 0xe85250d6
MULTIBOOT2_ARCH     equ 0
MULTIBOOT2_LEN      equ (header_end - header_start)
MULTIBOOT2_CHECKSUM equ -(MULTIBOOT2_MAGIC + MULTIBOOT2_ARCH + MULTIBOOT2_LEN)

section .multiboot_header
header_start:
    dd MULTIBOOT2_MAGIC
    dd MULTIBOOT2_ARCH
    dd MULTIBOOT2_LEN
    dd MULTIBOOT2_CHECKSUM
    ; required end tag
    dw 0    ; type
    dw 0    ; flags
    dd 8    ; size
header_end:

section .bss
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
[bits 32]
global _start

_start:
    mov esp, stack_top
    mov edi, ebx            ; save multiboot info pointer before cpuid clobbers ebx

    call check_long_mode_supported

    mov byte [0xb8000], 'B'
    mov byte [0xb8001], 0x0a
    mov byte [0xb8002], 'O'
    mov byte [0xb8003], 0x0a
    mov byte [0xb8004], 'O'
    mov byte [0xb8005], 0x0a
    mov byte [0xb8006], 'T'
    mov byte [0xb8007], 0x0a
    mov byte [0xb8008], 'O'
    mov byte [0xb8009], 0x0a
    mov byte [0xb800a], 'K'
    mov byte [0xb800b], 0x0a

    cli
.hang:
    hlt
    jmp .hang

; Halts with a red "ERR" pattern on VGA if the CPU lacks CPUID's
; extended long-mode leaf, or long mode itself isn't supported.
check_long_mode_supported:
    pushfd
    pop eax
    mov ecx, eax
    xor eax, 1 << 21
    push eax
    popfd
    pushfd
    pop eax
    push ecx
    popfd
    xor eax, ecx
    jz .no_long_mode

    mov eax, 0x80000000
    cpuid
    cmp eax, 0x80000001
    jb .no_long_mode

    mov eax, 0x80000001
    cpuid
    test edx, 1 << 29
    jz .no_long_mode
    ret

.no_long_mode:
    mov byte [0xb8000], 'E'
    mov byte [0xb8001], 0x4f
    mov byte [0xb8002], 'R'
    mov byte [0xb8003], 0x4f
    mov byte [0xb8004], 'R'
    mov byte [0xb8005], 0x4f
    cli
.hang2:
    hlt
    jmp .hang2
