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

# Login banner, from version.txt.
#
# Buildroot writes /etc/issue from BR2_TARGET_GENERIC_ISSUE, which is a literal
# string in the defconfig and so would be a second place to remember to bump.
# version.txt is the one place: "major minor", with the minor zero-padded to two
# digits the way the FRANK firmware numbers itself, so "1 0" reads v1.00.
VERSION_FILE="$BR2_EXTERNAL_FRANK_LINUX_PATH/../version.txt"
if [ -r "$VERSION_FILE" ]; then
    read -r VER_MAJOR VER_MINOR < "$VERSION_FILE"
    if [ "$VER_MINOR" -lt 10 ] 2>/dev/null; then
        VER="$VER_MAJOR.0$VER_MINOR"
    else
        VER="$VER_MAJOR.$VER_MINOR"
    fi
    printf 'FRANK Linux v%s\n' "$VER" > "$TARGET_DIR/etc/issue"
    echo "post-build: banner FRANK Linux v$VER"
else
    echo "post-build: no version.txt at $VERSION_FILE; leaving /etc/issue alone" >&2
fi

# Make the skeleton survive a read-only root.
#
# The rootfs is romfs in flash, executed in place. Buildroot's sysv skeleton
# assumes it can write to the root, and does so in two places that both fail
# here. Neither is configurable: BR2_TARGET_GENERIC_REMOUNT_ROOTFS_RW exists but
# only skeleton-init-systemd and skeleton-init-openrc read it, so for busybox
# init the line below is unconditional in the shipped inittab.
#
#   1. `mount -o remount,rw /` -- prints an error on every boot and cannot ever
#      succeed. romfs has no write path at all.
#
#   2. tmpfs for /tmp, /run and /dev/shm. TMPFS depends on SHMEM depends on MMU,
#      so it does not exist in this kernel, and `mount -a` skips all three. That
#      is invisible today only because the root is a writable initramfs and the
#      mount points are ordinary directories on it. On romfs they are read-only,
#      which takes /var with them -- Buildroot points var/log, var/spool,
#      var/cache and var/tmp at ../tmp.
#
# ramfs is the substitute: always built in, since it is what rootfs itself is on
# a kernel without SHMEM. It keeps the mode= option the /tmp and /dev/shm lines
# rely on, and drops only size=, which nothing here sets. That is the real cost:
# nothing bounds /tmp except the 8 MB the machine has.
#
# Both the device and the type column say "tmpfs", so this replaces the word
# wherever it appears rather than anchoring to the start of the line.
sed -i '/mount -o remount,rw \//d' "$TARGET_DIR/etc/inittab"
sed -i 's/\btmpfs\b/ramfs/g' "$TARGET_DIR/etc/fstab"
echo "post-build: inittab/fstab adjusted for a read-only root"

# seedrng keeps its seed in /var/lib/seedrng, the one part of /var that is a
# real directory rather than a symlink into /tmp. It is written to fail quietly,
# so this only saves a confusing message; the seed is not persistent either way,
# because /tmp does not survive a reboot. Persisting it would mean writing to
# the SD card on every boot, which is not worth it for a machine with no clock
# and no network.
mkdir -p "$TARGET_DIR/etc/default"
printf 'SEEDRNG_ARGS="--seed-dir=/tmp/seedrng --skip-credit"\n' \
    > "$TARGET_DIR/etc/default/seedrng"

# Mount points have to exist in the image: nothing can mkdir on a read-only root.
mkdir -p "$TARGET_DIR/mnt/sd"

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
    for t in mputest memtouch castest; do
        "$CC" -Os -Wall -o "$TARGET_DIR/usr/bin/$t" \
              "$BR2_EXTERNAL_FRANK_LINUX_PATH/board/frank/core2-proto/$t.c"
        "$STRIP" --strip-unneeded "$TARGET_DIR/usr/bin/$t"
    done
    echo "post-build: installed mputest memtouch"
else
    echo "post-build: no cross gcc found; test tools not built" >&2
fi

exit 0
