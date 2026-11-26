/* Run from a pinned page. Use registers only while the MMU is off. */
.syntax unified
.arch armv5te
.arm
.section .text, "ax", %progbits
.global handoff_start
.macro cpwait
    mrc p15, 0, r12, c2, c0, 0
    mov r12, r12
    sub pc, pc, #4
.endm
.macro sync_identity
    mcr p15, 0, r4, c7, c10, 4  /* drain the page-table store */
    mcr p15, 0, r4, c8, c5, 1   /* invalidate this I-TLB entry */
    mcr p15, 0, r4, c8, c6, 1   /* invalidate this D-TLB entry */
    cpwait
.endm

handoff_start:
    mrs r12, cpsr
    stmdb sp!, {r4-r11, r12, lr}
    orr r12, r12, #0xc0
    msr cpsr_c, r12
    mov r4, r0                  /* pinned page's physical address */
    mov r5, r1                  /* original virtual pointer to L1 slot */
    mov r6, r2                  /* temporary identity descriptor */
    ldmia r3, {r8-r11}          /* old descriptor, TTBR, FCSE PID, DACR */

    mov r0, #1
    mrc p15, 0, r7, c1, c0, 0
    tst r7, #1
    beq return_to_c
    mrc p15, 0, r1, c2, c0, 0
    cmp r1, r9
    bne return_to_c
    mrc p15, 0, r1, c13, c0, 0
    cmp r1, r10
    bne return_to_c
    mrc p15, 0, r1, c3, c0, 0
    cmp r1, r11
    bne return_to_c
    ldr r1, [r5]
    cmp r1, r8
    bne return_to_c

    /* Reuse WinCE's open COM1 configuration. Only the TX register is written. */
    ldr r9, =0xae100000
    mov r0, #2
    ldr r1, [r9, #12]
    tst r1, #0x80               /* divisor latch access must be disabled */
    bne return_to_c
    ldr r1, [r9, #4]
    tst r1, #0x40               /* UART unit enabled */
    beq return_to_c
    mov r0, #0x40               /* finish any pending WinCE transmission */
    bl wait_uart
    cmp r0, #0
    moveq r0, #2
    beq return_to_c

    str r6, [r5]
    sync_identity
    /* Verify that the new alias reaches the pinned page before branching. */
    adr r0, cookie
    adr r1, handoff_start
    sub r1, r0, r1
    ldr r2, [r4, r1]
    ldr r0, [r0]
    mov r11, #5
    cmp r0, r2
    bne resume_virtual
    adr r6, resume_virtual
    .ifdef BOOT_LINUX
    ldr r11, [r3, #16]          /* physical pointer to Linux scatter table */
    .endif
    adr r1, disable_mmu
    adr r2, handoff_start
    sub r1, r1, r2
    bic r0, r7, #5              /* disable the MMU and D-cache together */
    bic r0, r0, #0x1800         /* disable I-cache and branch prediction */
    add pc, r4, r1

    /* Both CP15 control writes begin on instruction-cache line boundaries. */
    .balign 32
disable_mmu:
    mcr p15, 0, r0, c1, c0, 0
    cpwait
    .ifndef BOOT_LINUX
    mov r11, #3
    .endif
    mrc p15, 0, r0, c1, c0, 0
    tst r0, #5
    .ifdef BOOT_LINUX
    bne linux_bad_mmu
    b linux_boot
    .else
    bne enable_mmu

    ldr r9, =0x40100000         /* physical FFUART */
    adr r10, marker
    mov r11, #4
send_byte:
    ldrb r3, [r10], #1
    cmp r3, #0
    beq drain_uart
    mov r0, #0x20
    bl wait_uart
    cmp r0, #0
    beq enable_mmu
    str r3, [r9]
    b send_byte
drain_uart:
    mov r0, #0x40
    bl wait_uart
    cmp r0, #0
    movne r11, #0
    b enable_mmu
    .endif

    .balign 32
enable_mmu:
    mcr p15, 0, r7, c1, c0, 0
    cpwait
    bx r6

resume_virtual:
    str r8, [r5]
    sync_identity
    mov r0, r11
return_to_c:
    ldmia sp!, {r4-r11, r12, lr}
    msr cpsr_c, r12
    bx lr

wait_uart:
    mov r2, #0x10000
poll_uart:
    ldr r1, [r9, #20]
    tst r1, r0
    bxne lr
    subs r2, r2, #1
    bne poll_uart
    mov r0, #0
    bx lr

    .ifdef BOOT_LINUX
    .include "linux_boot.inc"
    .endif
.ltorg
cookie:
    .word 0x3970ce55
    .ifndef BOOT_LINUX
marker:
    .asciz "H3970 RAM HANDOFF OK\r\n"
    .endif
