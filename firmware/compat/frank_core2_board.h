/*
 * frank_core2_board.h - compatibility shim.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The FRANK Core 2 Proto bring-up firmware has a stale include: five sources
 * (master/src/main.c, master/src/diag_link.c, slave/src/main.c,
 * common/mem_test.c, common/board_config.h) include "frank_core2_board.h",
 * but the file was renamed to "frank_core2_proto_board.h" and the include
 * sites were never updated. As it stands that firmware does not compile.
 *
 * We need it to build because it is the known-good target that validates this
 * repo's test harness -- proving the harness against firmware already known to
 * work, before trusting it to judge our own code.
 *
 * Fixing it properly means editing those five includes in
 * frank-lab/frank_core2_proto/firmware, which is a separate project and is not
 * committed to git there (the whole firmware/ directory is untracked), so an
 * edit would not be revertible. This shim goes on our own include path instead
 * and leaves that tree untouched.
 *
 * Delete this file once the upstream includes are corrected.
 */

#ifndef FRANK_CORE2_BOARD_COMPAT_H
#define FRANK_CORE2_BOARD_COMPAT_H

#include "frank_core2_proto_board.h"

#endif /* FRANK_CORE2_BOARD_COMPAT_H */
