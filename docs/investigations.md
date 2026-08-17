# Three questions, answered with measurements

Asked while the userspace was coming together: can the second RP2350 stand in
for an MMU, can this machine run a modern shell, and can it compile C.

Short answers: no, no, and not yet — but only one of the three is closed for
good, and the other two have a route that is a project rather than a wall.

---

## Can the master emulate an MMU for the slave?

**No, and not for a reason that better engineering could fix.**

An MMU translates addresses on every instruction fetch and every load and store,
inside the core, in the same cycle as the access. For the master to do that it
would have to sit between the slave's core and its memory. It cannot: the slave
fetches from its own PSRAM and flash through the QMI, on-chip. The 96 MiB/s link
is a message channel between two independent CPUs, not a memory bus.

The numbers make it worse than the topology does. The slave issues on the order
of 10^8 memory accesses per second. One link round trip costs 141 us idle and
2753 us with the master's USB HID host running (F3). Even if the link *were* in
the path, it would be five to six orders of magnitude too slow.

### What an MMU would actually buy, and where each stands

| want | can the master help? |
|---|---|
| `fork()` — child sees the same addresses, different memory | No. That is translation, in-core, by definition |
| demand paging, swap | No. Needs a fault on absent memory, which needs translation |
| protection | **Already have it.** PMSAv8 MPU, and `mputest` proves userspace cannot reach the kernel |
| virtual contiguity from fragmented physical memory | No. Translation again |

Three of the four need in-core translation and the fourth is already solved. The
MMU-shaped hole is smaller than it looks, and it got smaller again during this
work: the thing most badly wanted from real hardware — working atomics — turned
out to be solvable in the kernel with interrupt masking (F30), and `fork()` is
solvable by not needing it.

### What the master could usefully do instead

Serve a block device backed by its own 8 MB of PSRAM. The machinery exists — it
already serves the microSD over the link — and it would give `/tmp` somewhere to
live other than the slave's RAM. That matters: `/tmp` is unbounded ramfs today,
and a runaway write eats all 8 MB. It is not swap, and cannot be: swap needs
page faults.

---

## Can this machine run bash, or another modern shell?

**No.** See [shell-nommu.md](shell-nommu.md) for the full audit; the summary is
that a shell's child process *is* the shell.

Five of bash's seven `make_child()` sites keep interpreting after the fork, and
one of those is `$( … )`. A subshell needs a second copy of the interpreter
holding this shell's variables, functions, traps, options, parse position and
`$?`. No `exec` produces that process, because the state it needs was never on
disk, and re-exec would require bash to serialise its entire execution state.

mksh has exactly one process-creation site, which looks far more tractable until
you read the child branch: it ends in `execute(t, flags | XEXEC, NULL)`, straight
back into mksh's own tree walker. ash does not try — BusyBox marks it
`depends on !NOMMU` with a comment warning against removing the line.

BusyBox `hush` is the only shell in Buildroot that builds without an MMU, and
that is not about size: `shell/hush.c` carries about forty `#if !BB_MMU` blocks
and re-executes `/proc/self/exe` with the child's state passed on the command
line. It works because hush was written for this case rather than retrofitted.

**The one tractable improvement** is aliases, which hush does not have and which
have nothing to do with fork: they are a parse-time textual substitution. What
stops it is that hush's input reader is a `const char *p` and a two-character
peek buffer, so alias expansion needs a push-back layer in the parser of the one
program whose failure cannot be recovered without reflashing. Shell functions
cover most of the daily need and are shipped in `profile.d` today.

---

## Can this machine compile C?

**Not today, and the blocker is the instruction set rather than size.**

### tinycc cannot target this CPU

tcc is the obvious candidate — small, self-contained, designed to be a native
compiler on modest machines. It has an ARM backend. That backend emits **A32**,
and M-profile has no A32 at all:

```
$ grep -ric thumb tcc-0.9.27/arm-gen.c    ->  0
$ grep -ric thumb tcc-0.9.27/arm-link.c   ->  18
```

The 18 are `R_ARM_THM_*` relocation types, so the *linker* understands Thumb
objects; the *code generator* has no notion of it. Code tcc produced here would
be rejected exactly the way libffi's assembly was:

```
Error: selected processor does not support ARM opcodes
```

This is not a tcc oversight. Small compilers target A32 or x86 because Thumb-2
codegen is harder: variable-length encodings, a 3-bit register preference in
16-bit forms, and IT blocks instead of general predication.

### The alternatives, and why each is out

| candidate | target | verdict |
|---|---|---|
| gcc | correct ISA | `cc1` is **30.8 MB**. The whole rootfs partition is 11 MB and the machine has 8 MB of RAM |
| clang/LLVM | correct ISA | Larger again |
| cproc + QBE | amd64, arm64, riscv64 | No Thumb backend |
| chibicc, 8cc, 9cc | x86-64 | No |
| SDCC | z80, 8051, stm8 | Not ARM |
| PCC | ARM A32 | Same wall as tcc |

So the space is empty: nothing small targets Thumb-2, and the one thing that
does targets it at thirty megabytes.

### The two routes that exist

**A Thumb-2 backend for tcc.** This is the real answer and it is bounded rather
than open-ended: `arm-gen.c` is 2151 lines implementing about 28 backend
functions, and `arm64-gen.c` — a complete, from-scratch backend — is 1837. A
Thumb-2 backend is the same shape of work. It would want a cross-hosted test
loop first, because debugging a miscompiled instruction on a board whose only
console is another chip is not where that work should start.

**A C interpreter, for scripting rather than compilation.** picoc and c4 need no
code generation and would run today, but they buy something different from what
was asked for: a way to run C-shaped programs, not a way to build the system on
itself.

The third option is to keep cross-compiling, which is what happens now and is
not obviously wrong for a machine with 8 MB of RAM.
