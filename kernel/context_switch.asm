; kernel/context_switch.asm — minimal callee-saved context switch.
;
; void context_switch(uint64_t *old_rsp, uint64_t *new_rsp)
; System V: rdi = old_rsp, rsi = new_rsp
;
; Saves the outgoing task's callee-saved registers and RSP onto its
; own kernel stack, then loads the incoming task's saved RSP and pops
; its callee-saved registers. The final `ret` resumes execution
; wherever the incoming task last called context_switch from -- for a
; brand-new task, that's a fake frame set up by
; task_create_kernel_thread (or spawn(), from Task 5 on) rather than a
; real prior call.

section .text
[bits 64]
global context_switch

context_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15

    mov [rdi], rsp
    mov rsp, [rsi]

    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret
