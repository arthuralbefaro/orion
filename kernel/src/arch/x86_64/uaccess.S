.section .text

.global uaccess_copy
.type uaccess_copy, @function
uaccess_copy:
    movq %rdx, %rcx
.Lcopy_insn:
    rep movsb
    xorl %eax, %eax
    ret
.Lcopy_fix:
    movq %rcx, %rax
    ret

.global uaccess_strncpy
.type uaccess_strncpy, @function
uaccess_strncpy:
    xorl %eax, %eax
1:
    cmpq %rdx, %rax
    jae 2f
.Lstr_insn:
    movb (%rsi,%rax), %cl
    movb %cl, (%rdi,%rax)
    testb %cl, %cl
    jz 2f
    incq %rax
    jmp 1b
2:
    ret
.Lstr_fix:
    movq $-1, %rax
    ret

.section .ex_table, "a"
.balign 8
    .quad .Lcopy_insn, .Lcopy_fix
    .quad .Lstr_insn, .Lstr_fix
