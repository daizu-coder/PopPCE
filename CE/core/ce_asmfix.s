@ SPDX-License-Identifier: MIT
@ Copyright (c) 2026 daizu-coder
@ ce_asmfix.s - replacements for NDS BIOS services used by upstream asm.
@
@ CE/core/cefix.sed rewrites every "swi 0x090000" into
@     str lr,[sp,#-4]! / bl ceBiosDiv / ldr lr,[sp],#4

	.syntax unified
	.arm
	.text
	.align 2

	.global ceBiosDiv
@------------------------------------------------------------------------------
ceBiosDiv:
@ Drop-in for NDS BIOS SWI 09h (Div), signed 32-bit.
@ In:  r0 = numerator, r1 = denominator
@ Out: r0 = quotient, r1 = remainder (sign of numerator), r3 = |quotient|
@ All other registers preserved, matching the BIOS contract.
@ Division by zero: the BIOS never returns a meaningful value; this
@ returns r0 = 0, r1 = numerator, r3 = 0 instead of hanging.
@------------------------------------------------------------------------------
	stmfd sp!,{r2,r4,r5,r12}
	eor r12,r0,r1				@ bit31 = sign of quotient
	mov r4,r0					@ bit31 = sign of remainder
	cmp r0,#0
	rsbmi r0,r0,#0				@ r0 = |N|
	cmp r1,#0
	rsbmi r1,r1,#0				@ r1 = |D|
	beq divByZero

	mov r3,#0					@ quotient
	clz r2,r1
	clz r5,r0
	subs r2,r2,r5				@ bit shift so D's top bit lines up with N's
	blt divDone					@ |N| < |D|
	mov r1,r1,lsl r2
divLoop:
	cmp r0,r1
	subhs r0,r0,r1
	adc r3,r3,r3				@ q = q*2 + (N >= D)
	mov r1,r1,lsr#1
	subs r2,r2,#1
	bge divLoop
divDone:						@ r3 = |q|, r0 = |rem|
	movs r1,r4
	mov r1,r0
	rsbmi r1,r1,#0				@ remainder takes numerator's sign
	movs r12,r12
	mov r0,r3
	rsbmi r0,r0,#0
	ldmfd sp!,{r2,r4,r5,r12}
	bx lr

divByZero:
	mov r0,#0
	mov r1,r4
	mov r3,#0
	ldmfd sp!,{r2,r4,r5,r12}
	bx lr

	.global cePsgCodeChanged
@------------------------------------------------------------------------------
cePsgCodeChanged:
@ Called (via CE/core/cefix.sed) at the end of pcepsg.s:updateAmplitudes, which
@ has just rewritten volume immediates inside PCEPSGMixer. Syncs the caches
@ through ce_stubs.c:ceSyncPsgCode. Its caller PCEPSGMixer still needs
@ r0 (length), r1 (destination) and r12 (psgptr), so save what C may
@ clobber. Flags are not needed afterwards.
@------------------------------------------------------------------------------
	stmfd sp!,{r0-r3,r12,lr}
	bl ceSyncPsgCode
	ldmfd sp!,{r0-r3,r12,lr}
	bx lr

	.end
