linux_bad_mmu:
    mov r11, #3
    b enable_mmu

linux_boot:
    /* If WinCE still has a running channel, leave it alone and return. */
    ldr r0, =0x40000000
    mov r1, #16
linux_check_dma:
    ldr r2, [r0], #4
    tst r2, #0x80000000
    bne linux_dma_busy
    subs r1, r1, #1
    bne linux_check_dma

    /* The transition is now one-way. Cache lines belonging to WinCE must
     * never write back over the Linux image or its decompression workspace. */
    mov r0, #0
    mcr p15, 0, r0, c7, c7, 0
    mcr p15, 0, r0, c13, c0, 0
    cpwait
    ldr r1, =0x40600000
    str r0, [r1]                /* disable PXA USB device controller */
    ldr r1, =0x44000000
    ldr r2, [r1]
    bic r2, r2, #1
    str r2, [r1]                /* stop LCD scanout DMA, preserve panel power */

    ldmia r11!, {r4, r5, r6, r8} /* copy destination, zImage, DTB, page count */
    mov r0, r4
    mov r4, r5
    mov r5, r6
    mov r6, r0
    mov r10, #0xa0000000
linux_copy_page:
    ldrh r0, [r11], #2
    orr r0, r10, r0, lsl #12
    mov r1, #256
linux_copy_chunk:
    ldmia r0!, {r2, r3, r7, r12}
    stmia r6!, {r2, r3, r7, r12}
    subs r1, r1, #1
    bne linux_copy_chunk
    subs r8, r8, #1
    bne linux_copy_page
    mcr p15, 0, r0, c7, c10, 4
    cpwait

    ldr r9, =0x40100000
    adr r10, linux_marker
    ldr r0, [r5]
    ldr r1, =0xedfe0dd0        /* FDT magic, little-endian load */
    cmp r0, r1
    bne linux_bad_payload
    ldr r0, [r4, #0x24]
    ldr r1, =0x016f2818        /* ARM zImage magic */
    cmp r0, r1
    beq linux_send_byte
linux_bad_payload:
    adr r10, linux_bad_marker
    mov r4, #0
linux_send_byte:
    ldrb r3, [r10], #1
    cmp r3, #0
    beq linux_drain_uart
    mov r0, #0x20
    bl wait_uart
    cmp r0, #0
    beq linux_enter
    str r3, [r9]
    b linux_send_byte
linux_drain_uart:
    mov r0, #0x40
    bl wait_uart
linux_enter:
    cmp r4, #0
    beq linux_enter
    msr cpsr_c, #0xd3          /* SVC, IRQ and FIQ masked, ARM state */
    mov r0, #0
    mvn r1, #0                  /* use the DTB rather than a machine ID */
    mov r2, r5
    bx r4

linux_dma_busy:
    rsb r11, r1, #16
    add r11, r11, #0x100       /* 0x100 + active DMA channel */
    b enable_mmu

    .balign 4
linux_marker:
    .asciz "H3970 LINUX ENTRY\r\n"
linux_bad_marker:
    .asciz "H3970 BAD RAM IMAGE\r\n"
    .balign 4
