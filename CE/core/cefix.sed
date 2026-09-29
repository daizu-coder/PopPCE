# cefix.sed - upstream NitroGrafx .s (NDS/devkitARM, ELF) -> cegcc (PE/COFF)
# Applied to the C-preprocessed assembly by CE/Makefile before
# arm-mingw32ce-as sees it. Upstream sources stay unmodified.
# Run with GNU sed -E.

# ELF-only symbol typing/sizing. PE/COFF's .type takes a different
# operand form, and function/object typing has no effect there anyway.
s/^[[:space:]]*\.type[[:space:]]+[A-Za-z0-9_]+[[:space:]]*,?[[:space:]]*(STT_FUNC|STT_OBJECT|%function|%object).*$//
s/^[[:space:]]*\.size[[:space:]].*$//

# Code sections. Upstream keeps most of its variables inline in the code
# section and writes them PC-relative (300+ sites, e.g. "str r5,scanline"
# in VDC.s) - fine in NDS ITCM. cegcc's ld happens to mark PE .text
# writable already (characteristics 0xE0300020), but rather than depend on
# that default, all core code goes into its own explicitly writable +
# executable section .ngtext ("xw"; PE section names max 8 chars).
# Upstream selects its DS fast-memory sections under NDS/ARM9, which
# CE/Makefile defines on purpose: without them several files (pcepsg.s,
# Memory.s) emit no section directive at all after a .struct block and the
# code lands in the absolute section.
s/^[[:space:]]*\.section[[:space:]]+\.(itcm|iwram|ewram)([[:space:],;@].*)?$/\t.section .ngtext,"xw"/
s/^[[:space:]]*(\.section[[:space:]]+)?\.text([[:space:],;@].*)?$/\t.section .ngtext,"xw"/
s/^[[:space:]]*\.section[[:space:]]+\.dtcm([[:space:],;@].*)?$/\t.data/
s/^[[:space:]]*\.section[[:space:]]+\.sbss([[:space:],;@].*)?$/\t.section .bss/

# "blx label" is ARM->Thumb interworking into the DS build's Thumb C code.
# PE/COFF cannot express it, and the CE build compiles all C as ARM.
s/\bblx([a-z]{0,2})[[:space:]]+([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*($|[;@])/bl\1 \2 \3/

# "swi 0x090000" is the NDS BIOS Div call (r0/r1 -> r0=quot, r1=rem,
# r3=|quot|; everything else incl. lr preserved). On CE a SWI traps into
# the kernel, so call CE/core/ce_asmfix.s:ceBiosDiv instead. Every upstream
# call site is in a leaf routine that returns with "bx lr", so lr must
# survive the call - hence the push/pop around the bl.
s/^([[:space:]]*)swi[[:space:]]+#?0x090000(.*)$/\1str lr,[sp,#-4]!\n\1bl ceBiosDiv\n\1ldr lr,[sp],#4\2/

# ldrd/strd need an 8-byte aligned address on the ARM926 (alignment
# fault, i.e. a Datatype Misalignment exception on CE); the DS's ARM946
# did not care. Upstream has two PC-relative ones on inline variables:
# VDC.s "strd r0,r1,vdcRegPtrL" (offset 0x424 in VDC.o, so misaligned
# whatever the link order) and Shared/AsmExtra.s rndSeed0. Split every
# PC-relative ldrd/strd into two word accesses.
s/^([[:space:]]*)(ldr|str)d[[:space:]]+(r[0-9]+|lr|r1[0-2]),[[:space:]]*(r[0-9]+|lr|r1[0-2]),[[:space:]]*([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*($|[;@].*)$/\1\2 \3,\5\n\1\2 \4,\5+4 \6/

# The remaining ldrd/strd are register based on data that upstream keeps
# 8-aligned within its file (e.g. h6280ST1Func at h6280OpTable-56). PE/COFF
# records a section's alignment as the largest .align seen in it, which
# is only 4 for most files, so ld may place a file's .ngtext at 4 mod 8.
# Raise every file's .ngtext to 8 up front: at offset 0 the .balign adds
# no padding, so the upstream layout is unchanged.
1i\	.section .ngtext,"xw"\n\t.balign 8\n\t.text

# VDC.s state that CE/core/pce_render.c reads but upstream does not export:
# display width, and the not yet logged scroll / VDC CR values (the
# per-line log in scrollBuff is only filled up to vdcScrollLine /
# vdcCtrl1Line, later lines use the current scrollOld / vdcCtrl1Old).
# The _2 forms are the SuperGrafx's second VDC (core/mkvdc2.sh renames
# every VDC.s label before this script runs). vdcStat and vdcRegPtrL are
# for core/SgxCE.s (shared IRQ line, ST1/ST2 target).
s/^((vdcHDW|scrollOld|vdcScrollLine|vdcCtrl1Old|vdcCtrl1Line|vdcStat|vdcRegPtrL)(_2)?):/\t.global \1\n\1:/

# Per-line rendering: before VDC.s moves on to the next scanline, draw
# the one that just finished (CE/core/GfxCE.s:ceScanlineHook, which saves
# every register and the flags). Sprite patterns are streamed into VRAM
# while the screen is displayed (e.g. Street Fighter II), so drawing the
# whole frame at once from end-of-frame VRAM shows mixed-up sprites.
s/^VDCDoScanline:/VDCDoScanline:\n\tstr lr,[sp,#-4]!\n\tbl ceScanlineHook\n\tldr lr,[sp],#4/

# PCEPSG/pcepsg.s mixer rate. The channel counters advance PSGDIVIDE PSG
# clocks (3579545 Hz) per output sample: upstream's 80 means 44744 Hz,
# which is not a rate this device's waveOut plays reliably (see
# app/ce_audio.c kRateChoices). 81 gives 44192 Hz of PSG time per sample;
# played at 44100 Hz that is 0.2% (3.5 cents) flat, against 1.4% at 80.
# PSGDIVIDE is gone after cpp, so patch its expansion.
s/#0x00004000\*80\b/#0x00004000*81/

# pcepsg.s:updateAmplitudes writes the channel volumes straight into the
# mixer's instructions (self-modifying code; the DS keeps it in uncached
# ITCM). ARM926 has separate I/D caches, so tell CE after every rewrite
# (CE/core/ce_asmfix.s:cePsgCodeChanged, which saves every register it uses).
s/^([[:space:]]*)strb r8,\[psgptr,#amplitudeChg\](.*)$/&\n\1bl cePsgCodeChanged/

# pcepsg.s:_0805W stores a channel's L/R balance but, unlike $0801 and
# $0804, does not flag amplitudeChg, so the mixer keeps the old volume
# until something else touches $0801/$0804. A game that silences its
# music by writing balance 0 (Mai Nurse's opening) then holds the last
# notes forever. Flag the channel (r1 = channel, as in _0804W).
s/^([[:space:]]*)strb r0,\[r2,#ch0Balance\](.*)$/&\n\1mov r0,#1\n\1mov r0,r0,lsl r1\n\1ldrb r1,[psgptr,#amplitudeChg]\n\1orr r1,r1,r0\n\1strb r1,[psgptr,#amplitudeChg]/

# pcepsg.s:getVolumeDS adds up channel master + 2*channel balance +
# 2*global balance, so a balance nibble of 0 still leaves attenuation step
# 30 (about -45 dB), not silence. On the chip a balance of 0 mutes that side
# (Mednafen's scale_tab maps 0 to full attenuation). Games that end their
# music with balance 0 (Mai Nurse) otherwise keep a faint noise going.
# r3 = channel balance, r4 = global balance ror 4 (left nibble in bits 0-3,
# right in 28-31). r8/r9 are free: PCEPSGMixer reloads r4-r11 afterwards.
s/^getVolumeDS:.*$/&\n\tand r8,r3,#0xF0\n\ttst r4,#0x0F\n\tmoveq r8,#0\n\tand r9,r3,#0x0F\n\ttst r4,#0xF0000000\n\tmoveq r9,#0/
s/^([[:space:]]*)ldr r3,\[r5,r6,lsl#2\](.*)$/&\n\1cmp r8,#0\n\1moveq r2,#0\n\1cmp r9,#0\n\1moveq r3,#0/

# The 12 instructions updateAmplitudes patches (their low byte is the
# volume immediate). Global so CE/core/ce_stubs.c:ceSyncPsgCode can compare
# just these bytes instead of the whole mixer.
s/^(vol[0-5]_[LR]):/\t.global \1\n\1:/

# VDC.s marks changed VRAM in DIRTYTILES (1 byte per 128 bytes). Scanning
# all 512 bytes on every displayed line cost about 1.5 ms per frame on the
# device, although most lines see no VRAM write at all. Mirror each
# dirtymap write into CE/core/GfxCE.s:ceVramDirty (DIRTYTILES+0x200), so
# pce_render.c can skip the scan while nothing has changed. These are the
# only two dirtymap writes in the build (H6280extra.s is not built); the
# byte written has bits 5 and 7 clear (h6280a bits 0-23 = 0, r3 low byte
# = 0 in vramDMA).
s/^([[:space:]]*)strbpl h6280a,\[r1,r2,lsr#7\].*$/&\n\1strbpl h6280a,[r1,#0x200]/
s/^([[:space:]]*)strbpl r3,\[r6,r2,lsr#7\].*$/&\n\1strbpl r3,[r6,#0x200]/

# SuperGrafx (CE/core/SgxCE.s): a second VDC (VDC.s copied by
# core/mkvdc2.sh) and the VPC. Each entry below checks SgxCE.s:ceSgxOn
# first and goes straight to the upstream routine on a plain PC Engine.
#   cpu.s: both VDCs step every scanline and start every frame
s/^([[:space:]]*)bl (VDCDoScanline|newFrame)([[:space:]].*)?$/\1bl ceSgx_\2\3/
#   Cart.s bank tables, for pce_core_ce.c to map the SGX RAM banks
s/^(RDMEMTBL_|WRMEMTBL_):/\t.global \1\n\1:/
#   io.s $0000-$03FF: VDC1 / VPC / VDC2 by address bits 3-4
s/^([[:space:]]*)\.long (VDC_R|VDC_W)([[:space:]].*)?$/\1.long ceSgx_\2\3/
#   H6280.s ST0: goes to the VDC the VPC selects
s/^([[:space:]]*)bl VDC0W([[:space:]].*)?$/\1bl ceSgx_VDC0W\2/
#   VCE.s: a new dot clock or line count applies to both VDCs. Only the
#   VCE's "b calcHDW" follows the vdcLastScanline store (VDC.s's own one
#   in vdcHsr_H_W must stay).
/^[[:space:]]*str r1,\[r0\][[:space:]]*$/{
n
s/^([[:space:]]*)b calcHDW[[:space:]]*$/\1b ceSgx_calcHDW/
}

# cdrom.s: the drive's state block, saved by CE/core/pce_cd_ce.c
s/^(cdromState|cdromStateEnd|scsiCommandHex):/\t.global \1\n\1:/

# ArcadeCard.s: the Arcade Card registers (4 ports, the shift register),
# saved by CE/core/pce_cd_ce.c. Upstream's ".global ac_port0" names no label.
s/^(acPort0):/\t.global \1\n\1:/

# cdrom.s:updateCDROM runs the CD -> ADPCM RAM DMA 0xA00 bytes a frame, but
# only while dataLen != 0. When a transfer ends exactly on a frame's quota
# (e.g. 15 sectors = 12 x 0xA00), AdpcmDMA never gets to dmaEnd, so
# adpcmDmaOn stays set, $180C keeps reporting "busy writing" and the game
# waits forever (Garou Densetsu Special). Also call it while adpcmDmaOn is
# still set: dmaLoop then sees the SCSI status phase and runs dmaEnd.
s/^([[:space:]]*)ldrne r0,dataLen[[:space:]]*$/\1ldrne r0,dataLen\n\1ldrbne r1,adpcmDmaOn\n\1orrne r0,r0,r1/

# cdrom.s:CD0C_R ($180C) returns the ADPCM status as it was before the read:
# after a CPU write to $180A (or read), the first read of $180C still says
# "busy" (0x04/0x80) and only clears it for the next read. On hardware the
# CPU access finishes within a few cycles. Garou Densetsu Special copies
# Arcade Card RAM to ADPCM RAM a byte at a time, checking $180C bit 2 before
# each write and restarting the whole copy when it is set, so the copy never
# got past its first byte. Report busy only for what is still busy after
# the read (the CD -> ADPCM DMA).
s/^([[:space:]]*)strb r1,adpcmStatus[[:space:]]*$/\1strb r1,adpcmStatus\n\1and r0,r0,r1/

# VBL and a raster compare on the same line (VDC.s and its VDC2 copy).
# At line 240 vblHook sets the VBL status bit and the scanline hook then
# sets the RCR bit, both before the CPU runs, so an IRQ handler that tests
# VBL first (Kakutou Haou Densetsu Algunos: its raster chain ends with an
# RCR at line 240 that re-arms line 39) never sees that RCR, the chain
# stops and the split screen shows the wrong part of the BG. On the real
# chip the RCR comes first, a little before VBL. So when they coincide,
# vblHook leaves VBL primed (marked with bit 0) and the scanline hook
# raises it on the next line, after the CPU has had the RCR alone.
s/^vblHook(_2)?:.*$/&\n\tldr r0,vdcRasterCompareCPU\1\n\tcmp r5,r0\n\tbne 81f\n\tldrb r0,vdcCtrl1\1\n\ttst r0,#0x04\n\tbeq 81f\n\tldrb r0,vdcPrimedVBl\1\n\tcmp r0,#0\n\torrne r0,r0,#0x01\n\tstrbne r0,vdcPrimedVBl\1\n\tbx lr\n81:/
s/^noRasterIrq(_2)?:.*$/&\n\tldrb r0,vdcPrimedVBl\1\n\ttst r0,#0x01\n\tbeq 82f\n\tldr r2,vdcRasterCompareCPU\1\n\tcmp r5,r2\n\tbeq 82f\n\tmov r2,#0\n\tstrb r2,vdcPrimedVBl\1\n\tldrb r2,vdcStat\1\n\torr r2,r2,#0x20\n\tstrb r2,vdcStat\1\n\tsetIrqPin VDCIRQ_F\n82:/
