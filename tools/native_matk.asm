option casemap:none

.code

PUBLIC NativeMatkAdjust
PUBLIC ItemTableLookup
PUBLIC ItemProperCalc

; RCX = CharacterState, EAX = unadjusted M.Atk.
; Return the M.Atk with each equipped weapon's JOB_CORRECT factor applied.
NativeMatkAdjust PROC
    push rbx
    push rsi
    push rdi
    sub  rsp, 40h

    mov  rdi, rcx
    mov  esi, eax
    xor  ebx, ebx

    ; CharacterState.RHand and LHand contain ItemState pointers.
    mov  rax, qword ptr [rdi+78h]
    test rax, rax
    jz   short check_left
    mov  edx, dword ptr [rax+10h]
    test edx, edx
    jz   short check_left
    mov  dword ptr [rsp+20h], edx
    mov  ecx, edx
    xor  edx, edx
    call ItemTableLookup
    test rax, rax
    jz   short check_left
    cmp  dword ptr [rax+14h], 1       ; ItemTable.TYPE == weapon
    jne  short check_left
    mov  qword ptr [rsp+28h], rax
    mov  rcx, rdi
    mov  edx, dword ptr [rsp+20h]
    xor  r8d, r8d
    call ItemProperCalc
    sub  eax, 100
    mov  rdx, qword ptr [rsp+28h]
    imul eax, dword ptr [rdx+90h]     ; MATK * (JOB_CORRECT - 100)
    cdq
    mov  ecx, 100
    idiv ecx
    add  esi, eax

check_left:
    mov  rax, qword ptr [rdi+80h]
    test rax, rax
    jz   short done
    mov  edx, dword ptr [rax+10h]
    test edx, edx
    jz   short done
    mov  dword ptr [rsp+20h], edx
    mov  ecx, edx
    xor  edx, edx
    call ItemTableLookup
    test rax, rax
    jz   short done
    cmp  dword ptr [rax+14h], 1
    jne  short done
    mov  qword ptr [rsp+28h], rax
    mov  rcx, rdi
    mov  edx, dword ptr [rsp+20h]
    xor  r8d, r8d
    call ItemProperCalc
    sub  eax, 100
    mov  rdx, qword ptr [rsp+28h]
    imul eax, dword ptr [rdx+90h]
    cdq
    mov  ecx, 100
    idiv ecx
    add  esi, eax

done:
    mov  ebx, esi
    mov  eax, esi
    add  rsp, 40h
    pop  rdi
    pop  rsi
    pop  rbx
    ; Recreate the LEA overwritten at the call site: R8 = caller RSP + 38h.
    lea  r8, qword ptr [rsp+40h]
    ret
NativeMatkAdjust ENDP

; Calls are redirected by the patch builder to the game's own native methods.
ItemTableLookup PROC
    ret
ItemTableLookup ENDP

ALIGN 8
ItemProperCalc PROC
    ret
ItemProperCalc ENDP

END
