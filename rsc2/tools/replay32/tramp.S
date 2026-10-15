/* Enter guest code at an original Xbox VA with the snapshot's registers, and
 * come back here when it returns. 32-bit, AT&T syntax. */
    .text
    .globl run_guest
run_guest:
    pushal
    pushfl
    movl %esp, host_esp
    movl 40(%esp), %eax          /* entry VA: 4 ret + 32 pushal + 4 pushfl */
    movl %eax, entry_va
    cld
    movl guest_regs+16, %esp     /* guest esp */
    movl $ret_stub, (%esp)       /* the function returns to us, not its caller */
    movl guest_regs+4,  %ecx
    movl guest_regs+8,  %edx
    movl guest_regs+12, %ebx
    movl guest_regs+20, %ebp
    movl guest_regs+24, %esi
    movl guest_regs+28, %edi
    movups guest_xmm+0,   %xmm0
    movups guest_xmm+16,  %xmm1
    movups guest_xmm+32,  %xmm2
    movups guest_xmm+48,  %xmm3
    movups guest_xmm+64,  %xmm4
    movups guest_xmm+80,  %xmm5
    movups guest_xmm+96,  %xmm6
    movups guest_xmm+112, %xmm7
    fninit                       /* x87: the snapshot's control word, st(1), st(0) */
    fldcw guest_fpcw
    cmpl $2, guest_fpn
    jb 3f
    fldl guest_st+8
3:  cmpl $1, guest_fpn
    jb 4f
    fldl guest_st+0
4:
    movl guest_regs+0,  %eax
    cmpl $0, trace_tf
    je 1f
    pushfl                       /* single-step from the first guest instruction */
    orl $0x100, (%esp)
    popfl
1:
    jmp *entry_va
    .globl ret_stub
ret_stub:
    movl %eax, out_regs+0
    movl %ecx, out_regs+4
    movl %edx, out_regs+8
    movl %ebx, out_regs+12
    movl %esp, out_regs+16
    movl %ebp, out_regs+20
    movl %esi, out_regs+24
    movl %edi, out_regs+28
    movups %xmm0, out_xmm+0
    movups %xmm1, out_xmm+16
    movups %xmm2, out_xmm+32
    movups %xmm3, out_xmm+48
    movups %xmm4, out_xmm+64
    movups %xmm5, out_xmm+80
    movups %xmm6, out_xmm+96
    movups %xmm7, out_xmm+112
    fnsave out_fpu               /* the whole x87 state; leaves it reset */
    movl host_esp, %esp
    popfl
    popal
    ret

    .globl abort_to_host
abort_to_host:                   /* from the fault handler: unwind to host */
    movl host_esp, %esp
    popfl
    popal
    movl $-1, %eax
    ret

    .data
    .globl host_esp, entry_va, guest_regs, out_regs, trace_tf, guest_xmm, out_xmm
    .globl guest_fpcw, guest_fpn, guest_st, out_fpu
trace_tf:   .long 0
host_esp:   .long 0
entry_va:   .long 0
guest_regs: .fill 8, 4, 0
out_regs:   .fill 8, 4, 0
guest_xmm:  .fill 32, 4, 0
out_xmm:    .fill 32, 4, 0
guest_fpcw: .long 0x037F
guest_fpn:  .long 0
    .balign 8
guest_st:   .fill 2, 8, 0
out_fpu:    .fill 112, 1, 0
    .section .note.GNU-stack,"",@progbits
