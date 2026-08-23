; boot/boot.asm — Multiboot2 header, long-mode transition, and entry into the C kernel

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
align 4096
p4_table:
    resb 4096
p3_table:
    resb 4096
p2_table:
    resb 4096
align 16
stack_bottom:
    resb 16384
stack_top:

section .text
[bits 32]
global _start
extern kmain

_start:
    mov esp, stack_top
    mov edi, ebx            ; save multiboot info pointer before cpuid clobbers ebx

    call check_long_mode_supported
    call set_up_page_tables
    call enable_paging

    lgdt [gdt64.pointer]
    jmp gdt64.code:long_mode_start

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

; Identity-maps the first 1GiB using 2MiB pages: PML4[0] -> PDPT[0] -> 512 PD entries.
set_up_page_tables:
    mov eax, p3_table
    or eax, 0b11
    mov [p4_table], eax

    mov eax, p2_table
    or eax, 0b11
    mov [p3_table], eax

    mov ecx, 0
.map_p2_table:
    mov eax, 0x200000
    mul ecx
    or eax, 0b10000011      ; present + writable + huge page (2MiB)
    mov [p2_table + ecx * 8], eax

    inc ecx
    cmp ecx, 512
    jne .map_p2_table
    ret

enable_paging:
    mov eax, p4_table
    mov cr3, eax

    mov eax, cr4
    or eax, 1 << 5           ; PAE
    mov cr4, eax

    mov ecx, 0xC0000080      ; EFER MSR
    rdmsr
    or eax, 1 << 8           ; LME
    wrmsr

    mov eax, cr0
    or eax, 1 << 31          ; PG
    mov cr0, eax
    ret

section .rodata
gdt64:
    dq 0
.code: equ $ - gdt64
    dq (1<<43) | (1<<44) | (1<<47) | (1<<53)
.data: equ $ - gdt64
    dq (1<<41) | (1<<44) | (1<<47)
.pointer:
    dw $ - gdt64 - 1
    dq gdt64

section .text
[bits 64]
long_mode_start:
    mov edi, edi
    mov ax, gdt64.data
    mov ss, ax
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call kmain

    cli
.hang:
    hlt
    jmp .hang
