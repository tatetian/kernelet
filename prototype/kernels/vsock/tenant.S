# SPDX-License-Identifier: MPL-2.0
# A stackless tenant using scalar socket-control arguments and real data syscalls.
# Initial registers supply size, loops, role, peer CID, and port.
.global _start
.section .text
_start:
    mov $41, %eax
    syscall
    test %rax, %rax
    js fail
    mov %rax, %rbx
    cmp $4, %r14
    je recover
    cmp $1, %r14
    je client
    mov $49, %eax
    mov %rbx, %rdi
    mov %r8, %rsi
    syscall
    test %rax, %rax
    js fail
    mov $50, %eax
    mov $8, %esi
    syscall
    test %rax, %rax
    js fail
    cmp $3, %r14
    je stall
    mov $43, %eax
    syscall
    test %rax, %rax
    js fail
    mov %rax, %rbx
    cmp $2, %r14
    je stall
    cmp $5, %r14
    je faults
    cmp $9, %r14
    je produce_stall
    cmp $10, %r14
    je expect_zero_fin
    jmp server_round
client:
    mov $42, %eax
    mov %rbx, %rdi
    mov %r15, %rsi
    mov %r8, %rdx
    syscall
    test %rax, %rax
    js fail
client_round:
    xor %ebp, %ebp
client_write:
    mov $1, %eax
    mov %rbx, %rdi
    lea 0x10000000(%rbp), %rsi
    mov %r12, %rdx
    sub %rbp, %rdx
    syscall
    test %rax, %rax
    jle fail
    add %rax, %rbp
    cmp %r12, %rbp
    jb client_write
    xor %ebp, %ebp
client_read:
    xor %eax, %eax
    mov %rbx, %rdi
    lea 0x10000000(%rbp), %rsi
    mov %r12, %rdx
    sub %rbp, %rdx
    syscall
    test %rax, %rax
    jle fail
    add %rax, %rbp
    cmp %r12, %rbp
    jb client_read
    # Check every echoed byte against the original position-dependent pattern.
    xor %ebp, %ebp
verify:
    imul $17, %ebp, %eax
    add $3, %eax
    xor %edx, %edx
    mov $251, %esi
    div %esi
    cmp %dl, 0x10000000(%rbp)
    jne fail
    inc %rbp
    cmp %r12, %rbp
    jb verify
    dec %r13
    jnz client_round
    jmp done
server_round:
    xor %ebp, %ebp
server_read:
    xor %eax, %eax
    mov %rbx, %rdi
    lea 0x10000000(%rbp), %rsi
    mov %r12, %rdx
    sub %rbp, %rdx
    syscall
    test %rax, %rax
    jle fail
    add %rax, %rbp
    cmp %r12, %rbp
    jb server_read
    cmp $6, %r14
    jne server_echo
    xor %eax, %eax
    mov %rbx, %rdi
    mov $0x10000000, %esi
    mov $4096, %edx
    syscall
    test %rax, %rax
    jne fail
server_echo:
    xor %ebp, %ebp
server_write:
    mov $1, %eax
    mov %rbx, %rdi
    lea 0x10000000(%rbp), %rsi
    mov %r12, %rdx
    sub %rbp, %rdx
    syscall
    test %rax, %rax
    jle fail
    add %rax, %rbp
    cmp %r12, %rbp
    jb server_write
    dec %r13
    jnz server_round
done:
    mov $48, %eax
    mov %rbx, %rdi
    mov $1, %esi
    syscall
    test %rax, %rax
    js fail
    xor %edi, %edi
    mov $60, %eax
    syscall
fail:
    mov $1, %edi
    mov $60, %eax
    syscall
recover:
    mov $42, %eax
    mov %rbx, %rdi
    mov %r15, %rsi
    mov %r8, %rdx
    syscall
    test %rax, %rax
    js fail
    xor %eax, %eax
    mov %rbx, %rdi
    mov $0x10000000, %esi
    mov $4096, %edx
    syscall
    cmp $-104, %rax
    jne fail
    mov $3, %eax
    mov %rbx, %rdi
    syscall
    mov $41, %eax
    syscall
    test %rax, %rax
    js fail
    mov %rax, %rbx
    xor %r15d, %r15d
    mov $4321, %r8d
    jmp client
faults:
    # A mapped prefix followed by an unmapped page publishes only the prefix.
    mov $1, %eax
    mov %rbx, %rdi
    mov $0x1000f800, %esi
    mov $4096, %edx
    syscall
    cmp $2048, %rax
    jne fail
    mov $1, %eax
    mov $0x10010000, %esi
    mov $4096, %edx
    syscall
    cmp $-14, %rax
    jne fail
    # A zero-byte read fault must cancel its lease without consuming data.
    xor %eax, %eax
    mov $0x10010000, %esi
    mov $4096, %edx
    syscall
    cmp $-14, %rax
    jne fail
    xor %eax, %eax
    mov $0x1000f800, %esi
    mov $4096, %edx
    syscall
    cmp $2048, %rax
    jne fail
    xor %eax, %eax
    mov $0x10000000, %esi
    mov $2048, %edx
    syscall
    cmp $2048, %rax
    jne fail
    xor %ebp, %ebp
fault_verify:
    cmpb $0xb4, 0x10000000(%rbp)
    jne fail
    cmpb $0xb4, 0x1000f800(%rbp)
    jne fail
    inc %rbp
    cmp $2048, %rbp
    jb fault_verify
    mov $1, %eax
    mov %rbx, %rdi
    mov $0x10000000, %esi
    mov $2048, %edx
    syscall
    cmp $2048, %rax
    jne fail
    jmp done
stall:
    pause
    jmp stall
produce_stall:
    mov $1, %eax
    mov %rbx, %rdi
    mov $0x10000000, %esi
    mov $4096, %edx
    syscall
    cmp $4096, %rax
    jne fail
    jmp stall
expect_zero_fin:
    xor %eax, %eax
    mov %rbx, %rdi
    mov $0x10000000, %esi
    mov $4096, %edx
    syscall
    test %rax, %rax
    jne fail
    jmp done
