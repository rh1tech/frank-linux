/*
 * Minimal ARMv7-M boot wrapper for QEMU's MPS2 machines.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A Cortex-M does not have a jump-to-entry-point reset. It resets by reading
 * two words from the vector table at address 0 -- the initial stack pointer and
 * the reset handler -- and the kernel links at 0x21008000 with nothing at 0 at
 * all. Loading vmlinux alone therefore starts execution at address 0 and dies
 * immediately:
 *
 *     qemu: fatal: Lockup: can't escalate 3 to HardFault (current priority -1)
 *     R15=00000000
 *
 * So something has to sit at 0 and hand over. That is all this does, and it is
 * the same job afboot-rp2350 will do on real hardware -- there it also has to
 * bring up clocks, QMI and PSRAM first, but the handover is identical.
 *
 * The ARM boot protocol the kernel expects:
 *     r0 = 0
 *     r1 = machine type, or ~0 when booting from a device tree
 *     r2 = physical address of the DTB
 * and entry in Thumb state at the kernel's entry point.
 */

    .syntax unified
    .thumb

/*
 * Where things live. DRAM starts at 0x21000000 (CONFIG_DRAM_BASE in
 * mps2_defconfig) and the kernel links its text at 0x21008000, so the 32 KiB
 * below it is free and is where we put the device tree.
 */
    .equ KERNEL_ENTRY, 0x21008001    /* stext, with the Thumb bit set */
    .equ DTB_ADDR,     0x21000000
    .equ STACK_TOP,    0x21008000

/*
 * The vector table. Only the first two entries matter here: the hardware reads
 * the initial MSP from offset 0 and the reset handler from offset 4, before any
 * of our code runs.
 */
    .section .vectors, "a"
    .align 2
    .word STACK_TOP
    .word _reset + 1                 /* Thumb bit: an even address faults */

    .text
    .thumb_func
    .global _reset
_reset:
    /*
     * Enable the UART transmitter, and emit one character through it.
     *
     * This is not optional. Linux's mps2 earlycon writes to the data register
     * but never enables TX -- it assumes a bootloader already has. Without this
     * the entire boot log, panics included, disappears into a disabled UART and
     * the kernel looks like it never started. That silence cost a debugging
     * cycle here; the kernel had been booting the whole time.
     *
     * The 'W' is deliberate too: it arrives before the kernel prints anything,
     * so it distinguishes "the wrapper never ran" from "the kernel ran and said
     * nothing" -- which are otherwise the same silence from outside.
     *
     * UART0 is at 0x40004000 (serial@4000 in the DTB, off the 0x40000000 APB
     * base). DATA is at +0x00, CTRL at +0x08, and bit 0 of CTRL is TX enable.
     */
    ldr     r0, =0x40004000
    movs    r1, #1
    str     r1, [r0, #8]             /* CTRL: TX enable */
    movs    r1, #'W'
    str     r1, [r0, #0]             /* DATA */
    /*
     * Hand over exactly as a bootloader would. r1 is ~0 rather than a real
     * machine ID because this is a device-tree boot; the kernel takes the
     * machine from the DTB and ignores r1.
     */
    movs    r0, #0
    mvn     r1, #0
    ldr     r2, =DTB_ADDR
    ldr     r3, =KERNEL_ENTRY
    bx      r3

    /* Not reached. If the kernel ever returns, stop somewhere identifiable
     * rather than running off into whatever follows. */
.Lhang:
    b       .Lhang

    .end
