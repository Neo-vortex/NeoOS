; kernel/syscall_entry.asm — SYSCALL entry point (target of LSTAR).
;
; Entered with: RCX = user RIP (return address), R11 = user RFLAGS,
; RAX = syscall number, RDI/RSI/RDX/R10 = args 1-4. CS/SS are already
; switched to kernel selectors (via STAR); RSP is UNCHANGED (still the
; user stack) -- SYSCALL never switches stacks automatically.

extern syscall_dispatch
extern tss

section .bss
align 8
user_rsp_scratch: resq 1

section .text
[bits 64]
global syscall_entry

syscall_entry:
    ; Swap onto the current task's kernel stack. tss.rsp0 is kept up
    ; to date by the scheduler on every context switch (see
    ; process.c's schedule()), so it always names the right stack
    ; regardless of which task is running. tss_entry.rsp0 sits at
    ; offset 4 (right after the packed struct's 4-byte reserved0).
    mov [rel user_rsp_scratch], rsp
    mov rsp, [rel tss + 4]

    push qword [rel user_rsp_scratch] ; user RSP
    push rcx                           ; user RIP
    push r11                           ; user RFLAGS
    push rbx
    push rbp
    push r12
    push r13
    push r14
    push r15
    ; The register shuffle below and syscall_dispatch itself (an
    ; ordinary C function, free to clobber any SysV caller-saved
    ; register) will destroy the original argument registers. The only
    ; register a syscall is supposed to change from the caller's
    ; perspective is RAX (the return value) -- every userland syscall
    ; wrapper's clobber list (rcx, r11 only) relies on that convention,
    ; so save and restore the rest here rather than changing every
    ; wrapper's clobber list to document a leakier contract.
    push rdi
    push rsi
    push rdx
    push r10
    push r8
    push r9

    ; Safe now that we're on the kernel stack with everything saved --
    ; keeps the system preemptible during (potentially long) syscall
    ; processing, mirroring SFMASK's guarantee that only the brief
    ; stack-swap above ran with interrupts off.
    sti

    ; Reorder incoming syscall args (rax=num, rdi=a1, rsi=a2, rdx=a3,
    ; r10=a4) into SysV call registers for syscall_dispatch (rdi=num,
    ; rsi=a1, rdx=a2, rcx=a3, r8=a4). Safe to clobber rdi/rsi/rdx/r10
    ; here despite just having pushed their original values above --
    ; those pushes preserved copies on the stack; the registers
    ; themselves are free to reuse until the pops below.
    mov r9, rax
    mov rax, r10
    mov r10, rdx
    mov rdx, rsi
    mov rsi, rdi
    mov rdi, r9
    mov rcx, r10
    mov r8, rax

    call syscall_dispatch

    cli   ; mask again before restoring user state, mirroring SFMASK's entry guarantee
    pop r9
    pop r8
    pop r10
    pop rdx
    pop rsi
    pop rdi
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r11
    pop rcx
    pop qword [rel user_rsp_scratch]
    mov rsp, [rel user_rsp_scratch]

    o64 sysret
