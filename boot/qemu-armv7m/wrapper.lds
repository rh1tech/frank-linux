/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The vector table must land at address 0: that is where a Cortex-M reads its
 * initial stack pointer and reset handler at reset, before VTOR can be changed.
 * On QEMU's MPS2 machines address 0 is the boot ROM alias.
 */
ENTRY(_reset)

MEMORY
{
    ROM (rx) : ORIGIN = 0x00000000, LENGTH = 4M
}

SECTIONS
{
    .vectors : { KEEP(*(.vectors)) } > ROM
    .text    : { *(.text*) *(.rodata*) } > ROM
    /DISCARD/ : { *(.ARM.attributes) *(.comment) }
}
