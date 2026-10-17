.section .text

.extern syscall_dispatch

.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    swapgs
    movq %rsp, %gs:8
    movq %gs:0, %rsp

    pushq $0x1b
    pushq %gs:8
    pushq %r11
    pushq $0x23
    pushq %rcx
    pushq $0
    pushq $0x80

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

    cld
    movq %rsp, %rdi
    call syscall_dispatch

    cli
    testb %al, %al
    jz 1f
    movq %rsp, %rdi
    jmp user_return
1:
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

    addq $16, %rsp
    popq %rcx
    addq $8, %rsp
    popq %r11
    popq %rsp
    swapgs
    sysretq

.global user_return
.type user_return, @function
user_return:
    movq %rdi, %rsp
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
    addq $16, %rsp
    swapgs
    iretq
