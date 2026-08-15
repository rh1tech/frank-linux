# Phase 1: the RISC-V smoke test

Proves this board can host a Linux kernel in PSRAM before we write an ARM port
that has never been done — QMI setup, PSRAM timing, flash layout, console — and
leaves a known-good reference to diff against when the ARM kernel misbehaves.

It runs on the **master** half. That is the half whose UART console is wired to a
probe, and upstream's console is already UART0 on GPIO0/1, which is exactly the
master's J2 header. On the slave, GPIO0 *is* the PSRAM chip select, so UART0 does
not exist there at all.

## Why patches instead of a fork

The Buildroot external tree comes from
[Mr-Bossman/pi-pico2-linux](https://github.com/Mr-Bossman/pi-pico2-linux) by
Jesse Taube. **That repository carries no licence file**, so it is not ours to
redistribute. `make smoke-riscv` fetches it at a pinned commit and applies the
patches here, exactly as it fetches Buildroot.

It is also the better engineering. Our changes are 103 lines against a ~50-file
tree; as a fork they would be invisible, and every upstream fix would have to be
merged by hand.

Pinned to `29acd51afd263ce6aa6edeed54100746ad4d1044`.

## The patches

**0001 — PSRAM chip select.** Upstream hardcodes `RP2350_XIP_CSI_PIN 19`, the
SparkFun Pro Micro's pin, which matches neither half of this board: the master
(RP2350B) has its 8 MB PSRAM chip select on GPIO47, the slave (RP2350A) on
GPIO0. Makes the pin overridable and routes it from the Buildroot package, so
one tree serves both halves.

**0002 — post-image without picotool or a terminal.** Two problems in a
container. picotool is not installed, and building it there to produce a UF2 we
do not use — we flash the raw `.bin` over SWD — would be a lot of container for
nothing. Worse, the script's last command is `tput rmso`, which exits non-zero
when `TERM` is unset; because it is last, that becomes the script's exit status
and Buildroot fails `target-post-image` *after* writing the image successfully.

## Result

See [../docs/hw-findings.md](../docs/hw-findings.md) F8. It boots to an
interactive BusyBox shell — and cannot start a single external command, because
`binfmt_flat` needs 512 kB contiguous per process and 8 MB of RAM holding the
kernel image and initramfs cannot provide it. That measurement is what makes
`xipImage`-from-flash and FDPIC-with-shared-libc requirements rather than
preferences on the ARM side.
