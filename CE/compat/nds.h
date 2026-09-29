// SPDX-License-Identifier: MIT
// Copyright (c) 2026 daizu-coder
/*
 * nds.h - stand-in for libnds' header, for the upstream sources the CE
 * build compiles as they are (source/cueparser/CUEParser.c). They only
 * need the fixed-size integer types, bool and strlcpy (which the cegcc
 * C library lacks; defined in CE/core/pce_cd_ce.c).
 */
#ifndef CE_COMPAT_NDS_H
#define CE_COMPAT_NDS_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

size_t strlcpy(char *dst, const char *src, size_t size);

#endif
