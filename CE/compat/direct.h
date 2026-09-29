/* SPDX-License-Identifier: MIT */
/* Copyright (c) 2026 daizu-coder */
/*
 * Stand-in for <direct.h> on this cegcc toolchain, same reason as
 * compat/errno.h: the toolchain's own direct.h fails on a broken
 * #include_next chain. Nothing in this build calls the
 * _getcwd/_chdir/_mkdir family, so empty is enough.
 */
#ifndef CE_COMPAT_DIRECT_H
#define CE_COMPAT_DIRECT_H
#endif
