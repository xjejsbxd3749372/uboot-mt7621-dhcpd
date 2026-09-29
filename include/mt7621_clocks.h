/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Wrapper so that code outside the mach directory can reach
 * arch/mips/mach-mt7621/clocks.h.  The MIPS arch Makefile only adds the
 * per-machine "include" subdirectory to the include path, so the mach
 * directory itself is not otherwise reachable.
 */
#ifndef __MT7621_CLOCKS_WRAPPER_H
#define __MT7621_CLOCKS_WRAPPER_H

#include <../arch/mips/mach-mt7621/clocks.h>

#endif
