; Coroutine context switch (x64, Microsoft ABI). Used for the game's fibres and for the game's main
; thread so that save states can copy every stack (see costack.cpp).
;
;   void co_swap(void** save_rsp, void* new_rsp)
;       saves the non-volatile registers on the current stack, stores rsp into *save_rsp, switches to
;       new_rsp and restores the registers saved there. Returns on the other stack.
;   co_start: first "return" target of a fresh stack; calls r12(r13).

.code

co_swap PROC
    push rbp
    push rbx
    push rdi
    push rsi
    push r12
    push r13
    push r14
    push r15
    sub rsp, 168
    movaps [rsp], xmm6
    movaps [rsp+16], xmm7
    movaps [rsp+32], xmm8
    movaps [rsp+48], xmm9
    movaps [rsp+64], xmm10
    movaps [rsp+80], xmm11
    movaps [rsp+96], xmm12
    movaps [rsp+112], xmm13
    movaps [rsp+128], xmm14
    movaps [rsp+144], xmm15
    stmxcsr dword ptr [rsp+160]
    fnstcw word ptr [rsp+164]

    mov [rcx], rsp
    mov rsp, rdx

    movaps xmm6, [rsp]
    movaps xmm7, [rsp+16]
    movaps xmm8, [rsp+32]
    movaps xmm9, [rsp+48]
    movaps xmm10, [rsp+64]
    movaps xmm11, [rsp+80]
    movaps xmm12, [rsp+96]
    movaps xmm13, [rsp+112]
    movaps xmm14, [rsp+128]
    movaps xmm15, [rsp+144]
    ldmxcsr dword ptr [rsp+160]
    fldcw word ptr [rsp+164]
    add rsp, 168
    pop r15
    pop r14
    pop r13
    pop r12
    pop rsi
    pop rdi
    pop rbx
    pop rbp
    ret
co_swap ENDP

co_start PROC
    mov rcx, r13
    sub rsp, 32
    call r12
    int 3
co_start ENDP

END
