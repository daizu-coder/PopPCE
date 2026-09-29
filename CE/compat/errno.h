/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Stand-in for <errno.h> on this cegcc toolchain.
 *
 * /opt/cegcc/arm-mingw32ce/include/errno.h does `#include_next <errno.h>`
 * to hand off to coredll's own errno.h, but there is no further directory
 * in the search chain that provides one, so the include_next fails ("no
 * include path in which to search for errno.h").
 *
 * Upstream's source/cueparser/CUEParser.c includes <errno.h> but never
 * reads or writes `errno`, so this declaration is enough (nothing in
 * this build defines it).
 */
#ifndef CE_COMPAT_ERRNO_H
#define CE_COMPAT_ERRNO_H
extern int errno;
#endif
