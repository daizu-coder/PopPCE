#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 daizu-coder
# mkvdc2.sh - makes the SuperGrafx's second VDC from upstream source/VDC.s.
#
# Usage: mkvdc2.sh <preprocessed VDC.s> <cefix.sed> > VDC2.s
#
# Upstream VDC.s emulates exactly one HuC6270 and keeps all of its state
# inline in its own code (PC-relative), so a second chip is simply a
# second copy of the file with every label it defines renamed to
# <label>_2 (upstream stays unedited). The H6280 .struct offsets
# (h6280*) that its headers define are shared and keep their names.
#
# The copy then points its few outside references at VDC2's own buffers
# (CE/core/SgxCE.s) or at no-ops where they belong to VDC1 only, and goes
# through the same cefix.sed as every upstream file.
set -e
pre="$1"
cefix="$2"

rename=$(mktemp)
trap 'rm -f "$rename"' EXIT

{
	grep -oE '^[A-Za-z_][A-Za-z0-9_]*:' "$pre" | sed 's/:$//' | grep -v '^h6280'
	echo vdcStateSize
} | sort -u | awk '{ printf "s/\\b%s\\b/%s_2/g\n", $1, $1 }' > "$rename"

# Outside references (not labels of VDC.s, so not renamed above):
#   pceVRAM, DIRTYTILES, scrollBuff   VDC2's own VRAM, dirtymap, scroll log
#   clearDirtyTiles                   VDC2's dirtymap (vdcLoadState)
#   BG_SCALING_TBL/_TO_FIT            calcVBL's DS scaling values: dummies
#   setVDPMode                        sets yStart from VDC1's timing: no-op
#   paletteTxAll                      the palette is VCE's, VDC1 does it
#   gfxEndFrame                       VDC1's end of frame (pceEndFrame): no-op
#   redrawTiles                       DS tile conversion: no-op
#
# vblHook takes CPU time ("sub cycles,cycles,...") so the raster IRQ of
# the VBlank line comes 90 cycles before VBlank. Both VDCs share the one
# CPU, so only VDC1 may do that: VDC2's copies of those two lines (the
# only ones that subtract a "<<10" amount; eatcycles and the block
# transfer macros from H6280mac.h also subtract cycles and must stay)
# are dropped.
sed -E -f "$rename" "$pre" | sed -E \
	-e 's/\bpceVRAM\b/pceVRAM2/g' \
	-e 's/\bDIRTYTILES\b/DIRTYTILES2/g' \
	-e 's/\bscrollBuff\b/scrollBuff2/g' \
	-e 's/\bclearDirtyTiles\b/clearDirtyTiles2/g' \
	-e 's/\bBG_SCALING_TBL\b/vdc2ScalingTbl/g' \
	-e 's/\bBG_SCALING_TO_FIT\b/vdc2ScalingFit/g' \
	-e 's/\b(setVDPMode|paletteTxAll|gfxEndFrame|redrawTiles)\b/ceNopFunc/g' \
	-e '/^[[:space:]]*sub cycles,cycles,(r1,lsl#10|#90<<10)[[:space:];@]/d' \
	| sed -E -f "$cefix"
