// SPDX-License-Identifier: MIT
// Copyright (c) 2026 daizu-coder
//
//  SgxCE.s
//  NitroGrafx CE port - SuperGrafx: second VDC wiring and the VPC (HuC6202)
//
//  Upstream NitroGrafx has no SuperGrafx video (only the 32KB RAM and the
//  SGX_DEVICE flag). Here:
//    - VDC2 is upstream VDC.s copied with every label renamed to <label>_2
//      (core/mkvdc2.sh), with its own VRAM / dirtymap / scroll log below.
//    - The VPC's registers are kept here; pce_render.c mixes the two VDCs
//      with them, per line.
//    - core/cefix.sed points the upstream call sites at the ceSgx_* entries
//      below. Each one checks ceSgxOn first and, on a plain PC Engine,
//      branches straight to the upstream routine.
//
//  VPC behaviour follows Mednafen (beetle-supergrafx-libretro,
//  mednafen/pce_fast/vdc.cpp, vpc_mix_inner.inc).
//
//  This file is only run through cpp, not core/cefix.sed: cefix.sed turns
//  "bl VDCDoScanline" and friends into calls of the entries below, which
//  must not happen to the entries themselves.
//
#ifdef __arm__

#include "Shared/nds_asm.h"
#include "Equates.h"
#include "ARMH6280/H6280mac.h"

	.global ceSgxOn
	.global vpcRegs
	.global ceNopFunc
	.global sgxReset
	.global clearDirtyTiles2
	.global scrollBuff2
	.global pceVRAM2
	.global DIRTYTILES2
	.global ceVramDirty2
	.global vdc2ScalingTbl
	.global vdc2ScalingFit

	.global ceSgx_VDCDoScanline
	.global ceSgx_newFrame
	.global ceSgx_VDC_R
	.global ceSgx_VDC_W
	.global ceSgx_VDC0W
	.global ceSgx_calcHDW

	.syntax unified
	.arm

	.section .ngtext,"xw"
	.balign 8
;@----------------------------------------------------------------------------
ceSgx_VDCDoScanline:		;@ cpu.s frame loop. Out: r0 = VDC1's result
;@----------------------------------------------------------------------------
	ldrb r0,ceSgxOn
	cmp r0,#0
	beq VDCDoScanline
	stmfd sp!,{r0,lr}
	bl VDCDoScanline			;@ Draws the line that just ended (both VDCs)
	str r0,[sp]
	bl VDCDoScanline_2
	ldmfd sp!,{r0,lr}
	bx lr
;@----------------------------------------------------------------------------
ceSgx_newFrame:				;@ cpu.s, before line 0
;@----------------------------------------------------------------------------
	ldrb r0,ceSgxOn
	cmp r0,#0
	beq newFrame
	str lr,[sp,#-8]!
	bl newFrame_2
	ldr lr,[sp],#8
	b newFrame
;@----------------------------------------------------------------------------
ceSgx_calcHDW:				;@ VCE.s: new dot clock / 262 or 263 lines
;@----------------------------------------------------------------------------
	ldrb r0,ceSgxOn
	cmp r0,#0
	beq calcHDW
	ldr r0,=vdcLastScanline		;@ Keep both VDCs on the same frame length
	ldr r0,[r0]
	ldr r1,=vdcLastScanline_2
	str r0,[r1]
	str lr,[sp,#-8]!
	bl calcHDW_2
	ldr lr,[sp],#8
	b calcHDW
;@----------------------------------------------------------------------------
ceSgx_VDC0W:				;@ H6280.s ST0: to the VDC the VPC selects
;@----------------------------------------------------------------------------
	ldrb r1,vpcStMode
	cmp r1,#0
	beq VDC0W
	b VDC0W_2
;@----------------------------------------------------------------------------
ceSgx_VDC_R:				;@ io.s, $0000-$03FF. In: addy. Out: r0
;@----------------------------------------------------------------------------
;@ SuperGrafx: bits 3-4 of the address pick VDC1 ($00), the VPC ($08),
;@ VDC2 ($10) or nothing ($18), mirrored every $20.
	ldrb r1,ceSgxOn
	cmp r1,#0
	beq VDC_R
	and r1,addy,#0x18
	cmp r1,#0x08
	beq vpcR
	bhi vdc2R
	tst addy,#3
	bne VDC_R					;@ Data reads: VDC1 only
	str lr,[sp,#-8]!
	bl VDC_R					;@ Status read, clears the IRQ line
	b statusRead
vdc2R:
	cmp r1,#0x10
	movne r0,#0
	bxne lr
	tst addy,#3
	bne VDC_R_2
	str lr,[sp,#-8]!
	bl VDC_R_2
statusRead:
;@ Both VDCs drive the one IRQ1 line and VDC.s clears it on any status
;@ read: raise it again while the other VDC still has a cause pending
;@ (VDC.s only sets vdcStat bits whose IRQ is enabled).
	ldr r1,=vdcStat
	ldrb r1,[r1]
	ldr r2,=vdcStat_2
	ldrb r2,[r2]
	orrs r1,r1,r2
	ldrbne r2,[h6280ptr,#h6280IrqPending]
	orrne r2,r2,#VDCIRQ_F
	strbne r2,[h6280ptr,#h6280IrqPending]
	ldr lr,[sp],#8
	bx lr
;@----------------------------------------------------------------------------
vpcR:
	eatcycles 1
	and r1,addy,#7
	cmp r1,#6					;@ $0E (ST mode) and $0F read 0
	adrlo r2,vpcRegs
	ldrblo r0,[r2,r1]
	movhs r0,#0
	bx lr
;@----------------------------------------------------------------------------
ceSgx_VDC_W:				;@ io.s, $0000-$03FF. In: addy, r0 = value
;@----------------------------------------------------------------------------
	ldrb r1,ceSgxOn
	cmp r1,#0
	beq VDC_W
	and r1,addy,#0x18
	cmp r1,#0x08
	beq vpcW
	bhi vdc2W
	tst addy,#3
	bne VDC_W					;@ Data writes go to the selected register
	str lr,[sp,#-8]!
	bl VDC_W					;@ Register select, also sets ST1/ST2's target
	ldr lr,[sp],#8
	b stFuncFix
vdc2W:
	cmp r1,#0x10
	bxne lr
	tst addy,#3
	bne VDC_W_2
	str lr,[sp,#-8]!
	bl VDC_W_2
	ldr lr,[sp],#8
	b stFuncFix
;@----------------------------------------------------------------------------
vpcW:
	eatcycles 1
	and r1,addy,#7
	cmp r1,#6
	bxhi lr
	beq vpcStW
	cmp r1,#3					;@ Window widths are 10 bits
	cmpne r1,#5
	andeq r0,r0,#3
	adr r2,vpcRegs
	strb r0,[r2,r1]
	bx lr
vpcStW:
	and r0,r0,#1
	strb r0,vpcStMode
;@----------------------------------------------------------------------------
stFuncFix:
;@----------------------------------------------------------------------------
;@ VDC.s:VDC0W points the CPU's ST1/ST2 at its own chip's selected
;@ register; the VPC's ST mode decides which chip they really go to.
	ldrb r0,vpcStMode
	cmp r0,#0
	ldreq r2,=vdcRegPtrL		;@ vdcRegPtrH follows it
	ldrne r2,=vdcRegPtrL_2
	ldr r0,[r2]
	ldr r1,[r2,#4]
	str r0,[h6280ptr,#h6280ST1Func]
	str r1,[h6280ptr,#h6280ST2Func]
	bx lr
;@----------------------------------------------------------------------------
sgxReset:					;@ Called from GfxCE.s:gfxReset (both machines)
;@----------------------------------------------------------------------------
	stmfd sp!,{r4-r7,lr}

	ldr r0,=0x00001111			;@ Mednafen: priority 0x11,0x11, windows 0
	str r0,vpcRegs
	mov r0,#0
	str r0,vpcRegs+4

	bl vdcReset_2
	bl clearDirtyTiles2

	ldr r0,=SCROLLBUFF_2		;@ Same start values as GfxCE.s:defaultScroll
	mov r1,#0
	ldr r2,=0x01003C3C
	ldr r3,=0x80000080
	mov r4,#0
	mov r5,#264
resScrl2:
	stmia r0!,{r1-r4}
	subs r5,r5,#1
	bne resScrl2

	ldmfd sp!,{r4-r7,pc}
;@----------------------------------------------------------------------------
clearDirtyTiles2:			;@ All of VRAM2 changed (also ceVramDirty2)
;@----------------------------------------------------------------------------
	ldr r0,=DIRTYTILES2
	mov r1,#0
	mov r2,#0x204/4
	b memset_
;@----------------------------------------------------------------------------
ceNopFunc:
	bx lr

	.pool
;@----------------------------------------------------------------------------
ceSgxOn:		.byte 0			;@ Set by pce_core_ce.c for SuperGrafx games
				.skip 3
;@ VPC registers as the CPU sees them ($0008-$000E):
;@  +0/+1 priority (low/high byte), +2/+3 window 1 width, +4/+5 window 2
;@  width (10 bits, 0x40 = left edge), +6 ST0-ST2 target (1 = VDC2)
vpcRegs:		.long 0x00001111
				.long 0
vpcStMode = vpcRegs+6
scrollBuff2:	.long SCROLLBUFF_2	;@ Read by VDC2 (VDC.s's scrollBuff)

	.section .bss
	.align 8
pceVRAM2:
	.space 0x10000
	.space 128*16				;@ VDC.s fills the log backwards
SCROLLBUFF_2:
	.space 264*16
DIRTYTILES2:
	.space 0x200
ceVramDirty2:					;@ Must follow DIRTYTILES2 (cefix.sed writes DIRTYTILES2+0x200)
	.space 4
vdc2ScalingTbl:					;@ calcVBL_2 writes up to +0x23
	.space 0x24
vdc2ScalingFit:
	.space 12

;@----------------------------------------------------------------------------
	.end
#endif // __arm__
