# Shell environment for the FRANK console.
#
# TERM matters more than usual: the console is the master RP2350 running a
# VersaTerm-derived engine, and full-screen programs pick their escape sequences
# from terminfo by this name. vt102 is what that engine actually implements --
# claiming vt100 loses insert/delete-line, and claiming xterm promises colour
# handling and mouse reporting that is not there.
export TERM=vt102

# Tell the kernel how big the screen is.
#
# The console is a ring in SRAM, not a UART with a driver that knows anything
# about geometry, so TIOCGWINSZ returns 0x0 and every full-screen program falls
# back to its built-in default -- usually 80x24, which leaves the bottom row of
# an 80x25 display unused and the status line in the wrong place. The master
# renders exactly 80x25 (25 rows of a 16 px font letterboxed into 480 lines), so
# say so once here and let vi, nano and mc read it the normal way.
stty rows 25 cols 80 2>/dev/null

export PAGER=less
export EDITOR=vi

# The card goes last, after the flash. Everything in the image is executed in
# place out of romfs and costs no RAM to run; anything on the card has to be
# copied into memory first, because a FAT filesystem on a block device cannot
# give the kernel a direct mapping. So when a program exists in both places, the
# flash copy is the one you want.
export PATH=$PATH:/mnt/sd/bin

# Somewhere writable for programs that keep state.
#
# HOME is /root, which is on the read-only romfs, so anything following the XDG
# defaults (~/.config, ~/.cache, ~/.local/share) fails to save and, in mc's
# case, complains about it on every start. Point them at /tmp instead: none of
# this is worth persisting on a machine with no clock, and /tmp is ramfs.
export XDG_CONFIG_HOME=/tmp/config
export XDG_CACHE_HOME=/tmp/cache
export XDG_DATA_HOME=/tmp/share

# Two lines so a wrapped command does not scroll the prompt off an 80x25 screen.
export PS1='\w
# '

# Functions, not aliases.
#
# hush has no `alias` builtin at all -- it is on the "not implemented" list at
# the top of shell/hush.c, and sourcing a file that uses one prints
# "sh: can't execute 'alias'" on every login. It is also not something that
# falls out of a port: aliases are a parse-time substitution and would need a
# push-back layer in hush's input reader, which is `const char *p` plus a
# two-character peek buffer. See docs/shell-nommu.md.
#
# Functions do nearly the same job and hush has them. The difference that
# remains is that a function cannot expand to part of a command or take the
# place of a keyword -- `ll -a` works, `alias sudo='sudo '` has no equivalent.
ll()  { ls -lh "$@"; }
la()  { ls -lha "$@"; }
l()   { ls -1 "$@"; }
df()  { busybox df -h "$@"; }
free(){ busybox free -m "$@"; }
mem() { busybox free -m; cat /proc/buddyinfo; }

# Executable mappings, where a 0x10.. address means the code is being
# fetched from flash as it runs and costs no RAM, and 0x11.. means it was copied
# into memory. A plain grep rather than something that reformats: the addresses
# are the answer, and a mapping this misparsed would be worse than no tool.
xip() { grep ' r-xp ' "/proc/${1:-self}/maps"; }
