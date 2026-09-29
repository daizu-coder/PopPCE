//
//  GfxCE.s
//  NitroGrafx - Windows CE (SHARP Brain) replacement for source/Gfx.s
//
//  Based on source/Gfx.s,
//  Copyright © 2003-2026 Fredrik Ahlström. All rights reserved.
//  Used under the author's terms (non-commercial, see CE/LICENSING.md).
//
//  CE port changes: everything that drove the DS 2D hardware (VRAM
//  banks, BG/OAM registers, HBlank DMA scroll tables, window, extended
//  palettes, the DS tile/sprite converters) is removed. The picture is
//  drawn in software by CE/core/pce_render.c, one line at a time from
//  ceScanlineHook (called at the top of VDC.s:VDCDoScanline, inserted by
//  CE/core/cefix.sed), using the per-line scroll/control log that VDC.s keeps
//  in scrollBuff. The rest of VDC.s, VCE.s and the core is unchanged.
//
#ifdef __arm__

#include "Shared/nds_asm.h"
#include "Equates.h"
#include "ARMH6280/H6280.i"

	.global gTwitch
	.global gFlicker
	.global gGfxMask
	.global gScalingSet
	.global yStart
	.global aspectYStart
	.global DIRTYTILES
	.global ceVramDirty
	.global scrollBuff
	.global BG_SCALING_TO_FIT
	.global BG_SCALING_TBL
	.global pceVRAM
	.global EMUPALBUFF
	.global gfxState
	.global gColorValue
	.global sprCollision

	.global gfxInit
	.global gfxReset
	.global setupScaling
	.global setVDPMode
	.global paletteInit
	.global gammaConvert
	.global clearDirtyTiles
	.global midFrame
	.global endFrame
	.global ceScanlineHook

	.syntax unified
	.arm

	.section .text
	.align 2
;@----------------------------------------------------------------------------
defaultScroll:
	.long 0x00000000,0x01003C3C,0x80000080,0x00000000
;@----------------------------------------------------------------------------
gfxInit:					;@ (called from machineInit) only need to call once
;@----------------------------------------------------------------------------
	stmfd sp!,{lr}

	bl resetScrollBuffers
	bl vdcInit
	bl vceInit

	ldmfd sp!,{pc}
;@----------------------------------------------------------------------------
resetScrollBuffers:
;@----------------------------------------------------------------------------
	stmfd sp!,{r4-r7,lr}

	ldr r0,=SCROLLBUFF1
	adr r1,defaultScroll
	ldmia r1,{r4-r7}
	mov r2,#264
resScrlBuf:
	stmia r0!,{r4-r7}
	subs r2,r2,#1
	bne resScrlBuf

	ldmfd sp!,{r4-r7,pc}
;@----------------------------------------------------------------------------
gfxReset:					;@ Called with cpuReset
;@----------------------------------------------------------------------------
	stmfd sp!,{lr}

	ldr r0,=gfxState
	mov r1,#0
	mov r2,#3					;@ 3*4
	bl memset_					;@ Clear GFX regs

	bl vdcReset
	bl vceReset

	bl clearDirtyTiles
	bl resetScrollBuffers

	bl setupScaling
	bl setVDPMode

	bl sgxReset					;@ SgxCE.s: VDC2 and the VPC

	ldmfd sp!,{pc}

;@----------------------------------------------------------------------------
setupScaling:		;@ r0-r3, r12 modified.
;@----------------------------------------------------------------------------
;@ DS: loaded the BG/sprite affine scale values. The CE build always draws
;@ the PCE frame 1:1 and scales it in the frontend, so there is nothing to
;@ do. Still called from VDC.s:calcVBL.
	bx lr

BG_SCALING_TO_FIT:	;@ 1:1, 7:6, 5:4. Written by VDC.s:calcVBL, not used.
	.long 0x0150,0xFEB6,0x0080

;@ VDC.s:calcVBL writes into these three tables by offset from
;@ BG_SCALING_TBL (up to +0x20), keep them together and in this order.
BG_SCALING_TBL:
	.long 0xFFFF,0xFFFF,0xE000	;@ 0xE2AB, 0xDB6D=7:6, 224->192
BG_SCALING_WIN:
	.long 0x00C0,0x00C0,0x00C0
BG_SCALING_OFS:
	.long 0,0,0
;@----------------------------------------------------------------------------
paletteInit:		;@ r0-r3 modified.
;@ Called by ui.c:  void paletteInit(u8 gammaVal);
;@----------------------------------------------------------------------------
	ldr r1,=vceRGBYCbCr
	ldrb r1,[r1]
	cmp r1,#0
	beq vceInitPaletteMap
	stmfd sp!,{r4-r9,lr}
	ldr r6,=MAPPED_RGB
	mov r7,r0					;@ Gamma value = 0 -> 4
	ldrb r8,gColorValue			;@ Color value = 0 -> 4
	mov r4,#512*2
	sub r4,r4,#2
noMap:							;@ Map 0000000gggrrrbbb  ->  0bbbbbgggggrrrrr
	mov r0,r4,lsr#1
	mov r1,r8
	bl yPrefix
	mov r9,r0

	mov r1,r7
	mov r0,r9,lsr#16
	bl gammaConvert
	mov r5,r0

	mov r0,r9,lsr#8
	and r0,r0,#0xFF
	bl gammaConvert
	orr r5,r0,r5,lsl#5

	and r0,r9,#0xFF
	bl gammaConvert
	orr r5,r0,r5,lsl#5

	strh r5,[r6,r4]
	subs r4,r4,#2
	bpl noMap
	ldmfd sp!,{r4-r9,lr}
	bx lr

;@----------------------------------------------------------------------------
yPrefix:					;@ Takes gggrrrbbb, outputs bbbbbbbbggggggggrrrrrrrr
;@----------------------------------------------------------------------------
	and r3,r0,#0x007
	orr r3,r3,r3,lsl#6
	orr r3,r3,r3,lsr#3
	mov r3,r3,lsr#1				;@ Blue
	and r2,r0,#0x1C0
	orr r2,r2,r2,lsr#3
	orr r2,r2,r2,lsr#6
	mov r2,r2,lsr#1				;@ Green
	and r0,r0,#0x38
	orr r0,r0,r0,lsl#3
	orr r0,r0,r0,lsr#6
	mov r0,r0,lsr#1				;@ Red
;@----------------------------------------------------------------------------
yConvert:					;@ r0=Red, r1=color 0-4, r2=Green, r3=Blue
;@----------------------------------------------------------------------------
	stmfd sp!,{r4-r5}

	mov r12,#77
	mul r4,r12,r0				;@ Red
	mov r12,#151
	mla r4,r12,r2,r4			;@ Green
	mov r12,#29
	mla r4,r12,r3,r4			;@ Blue

	rsb r5,r1,#4
	mul r4,r5,r4				;@ B&W
	orr r0,r0,r0,lsl#8
	mla r0,r1,r0,r4
	mov r0,r0,lsr#10

	orr r3,r3,r3,lsl#8
	mla r3,r1,r3,r4
	mov r3,r3,lsr#10

	orr r2,r2,r2,lsl#8
	mla r2,r1,r2,r4
	mov r2,r2,lsr#10

	orr r0,r0,r2,lsl#8
	orr r0,r0,r3,lsl#16

	ldmfd sp!,{r4-r5}
	bx lr
;@----------------------------------------------------------------------------
gPrefix:
	orr r0,r0,r0,lsl#4
;@----------------------------------------------------------------------------
gammaConvert:	;@ Takes value in r0(0-0xFF), gamma in r1(0-4),returns new value in r0=0x1F
;@----------------------------------------------------------------------------
	rsb r2,r0,#0x100
	mul r3,r2,r2
	rsbs r2,r3,#0x10000
	rsb r3,r1,#4
	orr r0,r0,r0,lsl#8
	mul r2,r1,r2
	mla r0,r3,r0,r2
	mov r0,r0,lsr#13

	bx lr

	.pool

;@----------------------------------------------------------------------------
clearDirtyTiles:
;@----------------------------------------------------------------------------
;@ Marks all of VRAM as changed, so pce_render.c rebuilds its tile cache.
;@ Clears ceVramDirty (the word right after DIRTYTILES) along with it.
	ldr r0,=DIRTYTILES
	mov r1,#0
	mov r2,#0x204/4
	b memset_

gFlicker:		.byte 1
				.skip 2
gTwitch:		.byte 0

gColorValue:	.byte 4
gGfxMask:		.byte 0
				.skip 2

;@----------------------------------------------------------------------------
midFrame:					;@ Called at line 96
;@----------------------------------------------------------------------------
;@ DS: converted the sprite table for OAM here. Sprites come in step 3.
	bx lr

;@----------------------------------------------------------------------------
endFrame:					;@ Called just before screen end (~line 192)
;@----------------------------------------------------------------------------
;@ Called through VDC.s:VDCDoScanline with the H6280 state live in r3-r12,
;@ so everything is saved around the C call (14 registers keeps sp 8-byte
;@ aligned).
	stmfd sp!,{r0-r12,lr}

	bl newX						;@ Finish the scroll log up to this line

	ldr r1,=vdcBurst
	ldrb r0,[r1]
	cmp r0,#0
	mov r0,#0
	strb r0,[r1]

	ldreq r0,=0x0000			;@ If burstmode, wait until next frame with enabling bgr.
	moveq r1,#0					;@ 1?
	moveq addy,#239
	adr lr,vdcRet
	beq vdcCtrl1Finish
	bne newVDCCR
vdcRet:
;@--------------------------
	ldr r0,=0x0000				;@ Display off below the last line
	ldr r1,=vdcEndFrameLine
	ldr r1,[r1]
	mov addy,#239
	bl vdcCtrl1Finish
;@--------------------------
	bl pceEndFrame				;@ pce_render.c, every line is drawn by now

	ldmfd sp!,{r0-r12,lr}
	bx lr

;@----------------------------------------------------------------------------
ceScanlineHook:				;@ Top of VDC.s:VDCDoScanline, via cefix.sed
;@----------------------------------------------------------------------------
;@ scanline is still the line whose CPU time just ended: draw it if it is
;@ a displayed one. H6280 state lives in r3-r12 and the caller may rely
;@ on the flags, so save everything the C call can touch (8 registers
;@ keeps sp 8-byte aligned; r4 holds the flags across the call).
	stmfd sp!,{r0-r4,r5,r12,lr}
	mrs r4,cpsr
	ldr r0,=scanline
	ldr r0,[r0]
	ldr r1,=vdcEndFrameLine
	ldr r1,[r1]
	cmp r1,#240					;@ pceFrame height
	movhi r1,#240
	cmp r0,r1					;@ Unsigned: skips line -1 too
	bllo pceRenderLine			;@ pce_render.c, r0 = line
	msr cpsr_f,r4
	ldmfd sp!,{r0-r4,r5,r12,lr}
	bx lr

	.pool
;@----------------------------------------------------------------------------
setVDPMode:
;@----------------------------------------------------------------------------
;@ Only yStart is still used (cpu.s: L/R Y panning in 1:1 mode).
	mov r0,#0
	ldrb r1,gScalingSet
	cmp r1,#SCALED_FIT
	moveq r0,#1
	cmp r1,#SCALED_ASPECT
	moveq r0,#2

	ldr r2,=BG_SCALING_OFS
	ldr r1,[r2,r0,lsl#2]
	str r1,yStart

	bx lr
	.pool
;@----------------------------------------------------------------------------
scrollBuff:			.long SCROLLBUFF1	;@ Per-line log, written by VDC.s

gfxState:
yStart:
	.long 0
sprMemAlloc:
	.byte 0
sprMemReload:
	.byte 0
	.skip 6

aspectYStart:
	.byte 24
gScalingSet:
	.byte SCALED_ASPECT			;@ scalemode(saved display type), default scale to aspect
sprCollision:
	.byte 0x20
	.skip 1

	.section .bss
	.align 8
pceVRAM:
	.space 0x10000

;@ scrollBuff, 16 bytes per line (all written by VDC.s):
;@  +0: ((X + hOffset) & maskX) | ((Y - write line) << 16)
;@  +4: bit2-3 BG on, bit4-5 sprites on, bits 16-31 DS x scale (pixel clock)
;@  +8: DS window H (unused)
;@ VDC.s fills backwards from the current line, hence the padding before.
	.space 128*16				;@ Emptybuffer.
SCROLLBUFF1:
	.space 264*16				;@ Scrollbuffer.

EMUPALBUFF:						;@ 512 x BGR555, filled by VCE.s:paletteTxAll
	.space 0x400

	.align 2
DIRTYTILES:						;@ One byte per 128 bytes of VRAM, VDC.s writes 0 on change.
	.space 0x200				;@ bit5 = BG tiles cached (pce_render.c)
ceVramDirty:					;@ Must follow DIRTYTILES (cefix.sed writes it at DIRTYTILES+0x200).
	.space 4					;@ Byte 0 = VDC.s's dirtymap byte on every VRAM write (0xA0 clear = changed).

;@----------------------------------------------------------------------------
	.end
#endif // __arm__
