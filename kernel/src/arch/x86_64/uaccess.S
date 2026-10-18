.section .text

.macro USER_ON
    cmpb $0, smap_enabled(%rip)
    je .Lno_stac\@
    stac
.Lno_stac\@:
.endm

.macro USER_OFF
    cmpb $0, smap_enabled(%rip)
    je .Lno_clac\@
    clac
.Lno_clac\@:
.endm

.global uaccess_copy
.type uaccess_copy, @function
uaccess_copy:
    movq %rdx, %rcx
    USER_ON
.Lcopy_insn:
    rep movsb
    USER_OFF
    xorl %eax, %eax
    ret
.Lcopy_fix:
    USER_OFF
    movq %rcx, %rax
    ret

.global uaccess_strncpy
.type uaccess_strncpy, @function
uaccess_strncpy:
    USER_ON
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
    USER_OFF
    ret
.Lstr_fix:
    USER_OFF
    movq $-1, %rax
    ret

.section .ex_table, "a"
.balign 8
    .quad .Lcopy_insn, .Lcopy_fix
    .quad .Lstr_insn, .Lstr_fix
