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

# Two lines so a wrapped command does not scroll the prompt off an 80x25 screen.
export PS1='\w
# '

# No aliases: hush has no `alias` builtin, and sourcing this printed
# "sh: can't execute 'alias': No such file or directory" on every login.
