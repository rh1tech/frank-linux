#!/bin/sh
#
# Strip the target tree.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Buildroot will not do this for us: BR2_STRIP_strip `depends on BR2_BINFMT_ELF`,
# and we select BR2_BINFMT_FDPIC, so the whole strip step silently disappears.
# The option is simply absent from .config -- not set to n, absent -- which is a
# quiet way to end up shipping debug symbols.
#
# On a normal system that would be a size optimisation. On NOMMU it is a
# correctness one. ramfs there needs one *physically contiguous* allocation per
# file, so a large file is not merely wasteful, it can be impossible to unpack:
#
#   warn_alloc from __alloc_frozen_pages_noprof
#   __alloc_pages_noprof from ramfs_nommu_expand_for_mapping
#   ...
#   unpack_to_rootfs from do_populate_rootfs
#
# That is this exact tree failing to extract its own initramfs, because
# libgcc_s.so.1 was 2.7 MB unstripped -- 71% of the rootfs, and an order-10
# allocation. Stripped it is 99 kB.
#
# --strip-unneeded, not -s: it removes the symbol table while keeping .dynsym,
# which the dynamic loader needs to resolve anything at all.

set -e

TARGET_DIR="$1"
[ -n "$TARGET_DIR" ] || { echo "post-build: no target dir given" >&2; exit 1; }

STRIP=""
for cand in "$HOST_DIR"/bin/*-strip; do
    [ -x "$cand" ] && { STRIP="$cand"; break; }
done
[ -n "$STRIP" ] || { echo "post-build: no cross strip in $HOST_DIR/bin" >&2; exit 1; }

before=$(du -sk "$TARGET_DIR" | cut -f1)
stripped=0

# Ask `file` what each one is rather than trusting the path or extension: the
# tree holds shell scripts, symlinks and device nodes alongside the binaries,
# and strip on a script destroys it.
find "$TARGET_DIR" -type f -print | while read -r f; do
    case "$(LC_ALL=C file -b "$f" 2>/dev/null)" in
        *ELF*executable*|*ELF*shared\ object*)
            "$STRIP" --strip-unneeded "$f" 2>/dev/null || true
            ;;
    esac
done

after=$(du -sk "$TARGET_DIR" | cut -f1)
echo "post-build: stripped target, ${before} kB -> ${after} kB"

# Drop the network init script.
#
# This kernel has no networking at all -- there is nothing to put a packet on --
# but Buildroot's skeleton installs S40network regardless, so every boot ends
# with
#
#   Starting network: ip: socket: Function not implemented
#   FAIL
#
# on the console, which is the first thing anyone sees and suggests something is
# broken. Nothing is: the script is asking for a socket the kernel was never
# built to provide. Removing it is honest; making it print OK would not be.
rm -f "$TARGET_DIR/etc/init.d/S40network"

# Build the MPU probe into the image.
#
# It has to be cross-compiled and it is one file, so a full Buildroot package
# would be more machinery than the thing it builds. BR2_EXTERNAL gives the
# script the toolchain already.
CC=""
for cand in "$HOST_DIR"/bin/*-uclinuxfdpiceabi-gcc; do
    [ -x "$cand" ] && { CC="$cand"; break; }
done
if [ -n "$CC" ]; then
    "$CC" -Os -Wall -o "$TARGET_DIR/usr/bin/mputest" "$BR2_EXTERNAL_FRANK_LINUX_PATH/board/frank/core2-proto/mputest.c"
    "$STRIP" --strip-unneeded "$TARGET_DIR/usr/bin/mputest"
    echo "post-build: installed mputest"
else
    echo "post-build: no cross gcc found; mputest not built" >&2
fi

exit 0
