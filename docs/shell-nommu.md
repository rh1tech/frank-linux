# A better shell on NOMMU: the audit, and why the answer is no

The plan for a usable userspace had three targets: an editor, a file manager,
and a better shell than BusyBox `hush`. The first two landed. This is the
audit that was supposed to decide whether bash could land too, and the answer
it produced.

**Verdict: no-go, for bash and for every other POSIX shell.** Not because the
work is large, but because the thing being asked for is the one thing NOMMU
cannot do. `hush` plus the toolbox is the answer, and this document says
exactly what that costs.

## What the plan expected to find

> Classify every `fork()` in bash as *execs immediately* (vfork-able) or *needs
> a copy of the shell* (re-exec). If the second class is small and localised,
> build the re-exec machinery. If it is spread through `execute_cmd.c`, stop.

That framing assumes the count is what matters. It is not.

## bash 5.2.37

Every process bash creates goes through `make_child()`, which is a thin wrapper
over `fork()`. Seven call sites:

| site | what it creates | child's next move |
|---|---|---|
| `execute_cmd.c:5655` `execute_disk_command` | an external command | `shell_execve()` |
| `execute_cmd.c:4446` `execute_simple_command` | a command in a pipeline, redirected, or async | keeps interpreting — may be a builtin or a function |
| `execute_cmd.c:4110` `execute_null_command` | `> file` with no command | redirections, then `exit` |
| `execute_cmd.c:654` `execute_command_internal` | `( … )` | keeps interpreting |
| `execute_cmd.c:2424` `execute_coproc` | a coprocess | keeps interpreting |
| `subst.c:6536` `process_substitute` | `<( … )` | keeps interpreting |
| `subst.c:7009` `command_substitute` | `$( … )` | keeps interpreting |

Two of the seven exec or exit promptly and could be `vfork()`. Five keep
running bash — and one of those five is `$( … )`.

## The part that is not about counting

A shell's child process **is the shell**. That is not an implementation detail
of bash; it is what a subshell means. `$(date)` requires a second copy of the
interpreter, holding this shell's variables, functions, traps, options, current
position in the parse tree and `$?`, which then executes one node of that tree
and writes the result to a pipe. There is no exec that produces that process,
because the state it needs was never on disk.

So the re-exec approach — start a fresh copy of the binary and hand it the
context down a pipe — requires bash to serialise its entire execution state.
bash has no such facility, and adding one is not a port; it is a redesign, in a
codebase where every one of those five sites reads global state directly.

Nothing here is bash-specific:

- **mksh R59c** has *one* process-creation site, `exchild()` in `jobs.c`, which
  looks far more tractable until you read the child branch: it ends in
  `execute(t, flags | XEXEC, NULL)` — straight back into mksh's own tree
  walker. One call site, same requirement.
- **ash** does not even try. BusyBox marks it `depends on !NOMMU`, with a
  comment warning against removing the line.
- **dash, zsh, ksh93** are the same shape. Buildroot has no ksh93 package at
  all, so its `spawnveg()` abstraction — the reason the plan listed it as the
  fallback — would mean writing a package and doing the port.

Buildroot marks all of them `depends on BR2_USE_MMU # fork()`. That dependency
is a blanket stand-in in most packages and comes off once the call sites are
dealt with — which is exactly what was done for nano, glib and mc. For shells
it is not a stand-in. It is the requirement.

## Why hush is the exception

`hush` is the only shell in Buildroot that builds without an MMU, and it is not
an accident of size. `shell/hush.c` carries about forty `#if !BB_MMU` blocks:
on a system without fork it re-executes `/proc/self/exe` and passes the state
the child needs on the command line and through the environment, then the new
copy reconstructs enough of the shell to run one node.

It works because hush's state is small enough to describe that way, and because
it was written with this case in mind from the beginning rather than retrofitted.
That is the whole difference.

## What that costs, precisely

`hush` here is built with bash compatibility, brace expansion, job control,
history, functions, `local`, traps, arithmetic and the usual builtins. What it
does not have, and will not:

| missing | consequence |
|---|---|
| aliases | no `alias ll='ls -l'`. There is no `alias` builtin at all — sourcing a profile that uses one prints `sh: can't execute 'alias'` |
| arrays | `a=(1 2 3)` and `${a[1]}` are unavailable |
| process substitution | `diff <(a) <(b)` must become two temporary files |
| `[[ … ]]` | partially covered by bash-compat; not all operators |
| `$'…'` quoting | absent |

Command substitution, pipelines, subshells, loops, conditionals and functions
all work, because hush pays the re-exec cost for them.

The gap that shows up daily is aliases. Everything else has a mechanical
workaround; that one does not.

## If this is revisited

The tractable options, in the order they are worth trying:

1. **Teach hush aliases.** Aliases are a parse-time textual substitution and
   need no process at all, so nothing about NOMMU makes this hard. It is a
   BusyBox feature request, not a port.
2. **Accept a non-POSIX shell.** A shell that never needs a subshell copy —
   because it has no subshells — can run anywhere. That is a different tool,
   not a better bash.
3. **Give the kernel a real `fork()`.** On NOMMU this means copying the
   process's whole memory and relocating every pointer inside it, which is
   undecidable in general. This is why NOMMU Linux has never had one.

Option 3 is the one that keeps being suggested, including in this project, and
it is worth being clear that it is not merely unimplemented.
