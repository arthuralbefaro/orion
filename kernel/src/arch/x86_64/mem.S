.section .text

.global memcpy
.type memcpy, @function
memcpy:
    movq %rdi, %rax
    movq %rdx, %rcx
    rep movsb
    ret
.size memcpy, . - memcpy

.global memset
.type memset, @function
memset:
    movq %rdi, %r8
    movl %esi, %eax
    movq %rdx, %rcx
    rep stosb
    movq %r8, %rax
    ret
.size memset, . - memset

.global memmove
.type memmove, @function
memmove:
    movq %rdi, %rax
    movq %rdx, %rcx
    cmpq %rsi, %rdi
    jbe 1f
    leaq (%rsi,%rdx), %r8
    cmpq %r8, %rdi
    jae 1f
    leaq -1(%rdi,%rdx), %rdi
    leaq -1(%rsi,%rdx), %rsi
    std
    rep movsb
    cld
    ret
1:
    rep movsb
    ret
.size memmove, . - memmove