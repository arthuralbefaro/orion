#define ISR_STUB_SIZE 16
#define MSR_GS_BASE 0xC0000101

.section .text

.extern isr_dispatch

.macro SAVE_REGS
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
.endm

.macro RESTORE_REGS
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
.endm

.macro KERNEL_ENTRY
    cld
    cmpb $0, smap_enabled(%rip)
    je 9f
    clac
9:
.endm

isr_common:
    SAVE_REGS
    testq $3, 144(%rsp)
    jz 1f
    swapgs
1:
    KERNEL_ENTRY
    movq %rsp, %rdi
    call isr_dispatch

    testq $3, 144(%rsp)
    jz 2f
    swapgs
2:
    RESTORE_REGS
    addq $16, %rsp
    iretq

isr_paranoid:
    SAVE_REGS
    xorl %r12d, %r12d
    movl $MSR_GS_BASE, %ecx
    rdmsr
    testl %edx, %edx
    js 1f
    swapgs
    movl $1, %r12d
1:
    KERNEL_ENTRY
    movq %rsp, %rdi
    call isr_dispatch

    testl %r12d, %r12d
    jz 2f
    swapgs
2:
    RESTORE_REGS
    addq $16, %rsp
    iretq

.align ISR_STUB_SIZE
.global isr_stubs_begin
isr_stubs_begin:

.set vec, 0
.rept 256
    .align ISR_STUB_SIZE
    .if (vec == 8) || (vec == 10) || (vec == 11) || (vec == 12) || (vec == 13) || \
        (vec == 14) || (vec == 17) || (vec == 21) || (vec == 29) || (vec == 30)
        pushq $vec
    .else
        pushq $0
        pushq $vec
    .endif
    .if (vec == 2) || (vec == 8)
        jmp isr_paranoid
    .else
        jmp isr_common
    .endif
    .set vec, vec + 1
.endr

.global isr_stubs_end
isr_stubs_end:
