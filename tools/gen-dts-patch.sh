#!/usr/bin/env bash
#
# gen-dts-patch.sh - regenerate the device-tree patch from the board sources.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The device tree exists twice: as editable files under br-external/board/, and
# as added-file hunks inside br-external/patches/linux/0005-*.patch, which is
# what the kernel actually builds. Editing the first and forgetting the second
# produces a build that succeeds and a DTB without the change in it -- the node
# simply never appears, and the driver you were expecting never probes, with no
# error anywhere to say why. That cost an afternoon once.
#
# So the board copy is the source and this regenerates the patch from it.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"
cd "$REPO"

DTS_DIR=br-external/board/frank/core2-proto/dts
PATCH=br-external/patches/linux/0005-arm-dts-frank-core2-proto.patch

[ -d "$DTS_DIR" ] || { echo "no $DTS_DIR" >&2; exit 1; }
[ -f "$PATCH" ] || { echo "no $PATCH" >&2; exit 1; }

# Everything before the first file header is the commit message; keep it.
header="$(sed -n '1,/^--- /p' "$PATCH" | sed '$d')"

tmp_mk="$(mktemp)"
trap 'rm -f "$tmp_mk"' EXIT

emit_added_file() {
    # $1 = path inside the kernel tree, $2 = source file.
    #
    # A real file, never a process substitution: this reads the source twice,
    # once to count lines and once for the body, and a <(...) pipe can only be
    # read once. The second read comes back empty, which produces a hunk header
    # promising lines that are not there -- "malformed patch at line N".
    local path="$1" src="$2" n
    [ -f "$src" ] || { echo "emit_added_file: $src is not a regular file" >&2; exit 1; }
    n=$(grep -c '' "$src")
    printf -- '--- /dev/null\n+++ b/%s\n@@ -0,0 +1,%d @@\n' "$path" "$n"
    sed 's/^/+/' "$src"
}

{
    printf '%s\n' "$header"

    # The one existing file this touches: a single line in the dts Makefile.
    sed -n '/^--- a\/arch\/arm\/boot\/dts\/Makefile$/,/^--- \/dev\/null$/p' "$PATCH" | sed '$d'

    # shellcheck disable=SC2016  # $(CONFIG_ARCH_RP2350) is a make variable for
    # the kernel's own Makefile, and must reach it unexpanded.
    printf '# SPDX-License-Identifier: GPL-2.0\ndtb-$(CONFIG_ARCH_RP2350) += frank-core2-proto-slave.dtb\n' > "$tmp_mk"
    emit_added_file "arch/arm/boot/dts/frank/Makefile" "$tmp_mk"
    emit_added_file "arch/arm/boot/dts/frank/rp2350.dtsi" "$DTS_DIR/rp2350.dtsi"
    emit_added_file "arch/arm/boot/dts/frank/frank-core2-proto-slave.dts" \
        "$DTS_DIR/frank-core2-proto-slave.dts"
} > "$PATCH.new"

mv "$PATCH.new" "$PATCH"
echo "regenerated $PATCH from $DTS_DIR"
