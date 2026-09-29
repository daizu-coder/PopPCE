# SPDX-License-Identifier: MIT
# Copyright (c) 2026 daizu-coder
#
# CE/Makefile - NitroGrafx core -> SHARP Brain (Windows CE, cegcc)
#
# Builds, from the upstream core .s files (assembled for PE/COFF) with
# source/Gfx.s (NDS 2D hardware) replaced by core/GfxCE.s + core/pce_render.c
# (software renderer):
#   AppMain.exe        the app: PopPCE's Win32 frontend (app/) driving
#                      the core through core/pce_core_ce.c (libretro-shaped)
#
# Layout: app/ frontend, core/ CE side of the core, include/ + compat/
# headers. Every intermediate file goes to build/.
#
# Upstream .s files are never edited. Each one goes through:
#   cpp -DNDS -DARM9 (upstream's DS section choices, see core/cefix.sed)
#   -> sed -E -f core/cefix.sed (ELF-only directives, blx, BIOS swi)
#   -> arm-mingw32ce-as
# The intermediate .s lands in build/ next to its .o for inspection.

TOOLPREFIX := /opt/cegcc/bin/arm-mingw32ce-
CC      := $(TOOLPREFIX)gcc
AS      := $(TOOLPREFIX)as
NM      := $(TOOLPREFIX)nm
STRIP   := $(TOOLPREFIX)strip

# armv5te (the PW-G5200's ARM926EJ-S) + no unaligned access.
ARCH_FLAGS := -march=armv5te -mno-unaligned-access
# gas only takes the -march half.
AS_FLAGS   := -march=armv5te

SRC   := ../source
BUILD := build
CORE  := core
CEFIX := $(CORE)/cefix.sed
TARGET := AppMain.exe

# UI bitmap font (app/ce_bmpfont.c). Galmuri14 is the default since the
# 2026-09-27 on-device comparison; the others stay buildable:
#   make                     Galmuri14 (default)        -> AppMain.exe
#   make CE_FONT=shinonome   Shinonome 16dot            -> AppMain_shinonome.exe
#   make CE_FONT=galmuri11   GalmuriMono11              -> AppMain_galmuri11.exe
# app/ce_galmuri14.h / ce_galmuri11.h come from tools/galmuri2c.py (committed;
# Python is not run by the build). The font define reaches every frontend
# file (ce_bmpfont.h's CE_BMPFONT_ROW_EXTRA), so each font builds the
# frontend into its own build/app_<font>/ and never reuses a stale object.
CE_FONT ?= galmuri14
ifeq ($(CE_FONT),galmuri14)
FONT_DEFS := -DCE_FONT_GALMURI14
else ifeq ($(CE_FONT),shinonome)
FONT_DEFS :=
TARGET := AppMain_shinonome.exe
else ifeq ($(CE_FONT),galmuri11)
FONT_DEFS := -DCE_FONT_GALMURI11
TARGET := AppMain_galmuri11.exe
else
$(error CE_FONT must be galmuri14, shinonome or galmuri11)
endif
APP_BUILD := $(BUILD)/app_$(CE_FONT)

# SAMPLE_PLAYING: same as upstream's own Makefile. The PSG is mixed a few
# samples per scanline (cpu.s -> Sound.s:soundUpdate) into a ring that
# soundRender drains; without it the mixer only runs once per frame and
# DDA (sampled) channels are muted entirely (pcepsg.s:getVolumeDS), which
# silences most sound effects and voices.
ASM_DEFINES  := -DNDS -DARM9 -DSAMPLE_PLAYING
ASM_INCLUDES := -I$(SRC) -I$(SRC)/ARMH6280 -I$(SRC)/Shared -I$(SRC)/PCEPSG -I../include

# Fail early, with a clear message, when the submodules were not checked
# out (git clone without --recursive, or a GitHub "Download ZIP") - the
# build would otherwise stop much later on a missing include.
ifneq ($(MAKECMDGOALS),clean)
ifeq ($(wildcard $(SRC)/ARMH6280/H6280.s),)
$(error source/ARMH6280 is empty - clone with --recursive, or run: git submodule update --init)
endif
ifeq ($(wildcard $(SRC)/Shared/AsmExtra.s),)
$(error source/Shared is empty - clone with --recursive, or run: git submodule update --init)
endif
endif

# Upstream core assembly. Gfx.s is not in the list: core/GfxCE.s replaces it.
# ARMH6280/H6280extra.s (per-game CPU speed hacks) is left out: its only
# entry point h6280Hacks is never called upstream (commented out in
# cpu.s:cpuReset), and it references symbols nothing defines (PCE_VRAM).
CORE_ASM := \
	ARMH6280/H6280.s \
	cpu.s \
	Memory.s \
	io.s \
	Cart.s \
	VDC.s \
	VCE.s \
	Sound.s \
	PCEPSG/pcepsg.s \
	ADPCM.s \
	cdrom.s \
	ArcadeCard.s \
	Shared/AsmExtra.s

CORE_OBJS := $(addprefix $(BUILD)/,$(subst /,_,$(CORE_ASM:.s=.o)))

# CE-side assembly that includes upstream headers: same cpp + cefix.sed
# pipeline as the core.
CE_CORE_OBJS := $(BUILD)/GfxCE.o

# SuperGrafx: VDC2 = source/VDC.s with its labels renamed (core/mkvdc2.sh),
# SgxCE.s = VPC and the entries cefix.sed routes the core through (cpp
# only, see the file).
SGX_OBJS := $(BUILD)/VDC2.o $(BUILD)/SgxCE.o

# CE side of the core written in C/asm
CE_OBJS := $(BUILD)/ce_asmfix.o $(BUILD)/pce_render.o $(BUILD)/ce_stubs.o

# CD-ROM: image access (core/pce_cd_ce.c) and upstream's cue sheet parser,
# built as it is with compat/nds.h standing in for libnds.
CD_OBJS := $(BUILD)/pce_cd_ce.o $(BUILD)/CUEParser.o

# Frontend (the same shape as the sister Pop ports' frontends, see
# app/ce_main.c)
APP_SRC := ce_main.c ce_log.c ce_display.c ce_input.c ce_audio.c ce_video.c \
	ce_config.c ce_lang.c ce_bmpfont.c ce_fileopen.c
APP_OBJS := $(addprefix $(APP_BUILD)/,$(APP_SRC:.c=.o)) $(APP_BUILD)/ce_res.o \
	$(BUILD)/pce_core_ce.o $(CD_OBJS)

CFLAGS  := $(ARCH_FLAGS) -O2 -Wall -fno-strict-aliasing
APP_CFLAGS := $(CFLAGS) $(FONT_DEFS) -Iapp -Icompat -Iinclude -I$(CORE)
LDFLAGS := -static-libgcc
WINDRES := $(TOOLPREFIX)windres

.PHONY: all clean undefined strip

all: $(TARGET)

$(BUILD):
	mkdir -p $@

# build/<dir>_<name>.o <- ../source/<dir>/<name>.s
define CORE_RULE
$(BUILD)/$(subst /,_,$(1:.s=.o)): $(SRC)/$(1) $(CEFIX) | $(BUILD)
	$(CC) -E -x assembler-with-cpp $(ASM_DEFINES) $(ASM_INCLUDES) -I$(dir $(SRC)/$(1)) $$< \
		| sed -E -f $(CEFIX) > $(BUILD)/$(subst /,_,$(1))
	$(AS) $(AS_FLAGS) $(ASM_INCLUDES) -o $$@ $(BUILD)/$(subst /,_,$(1))
endef
$(foreach f,$(CORE_ASM),$(eval $(call CORE_RULE,$(f))))

$(CE_CORE_OBJS): $(BUILD)/%.o: $(CORE)/%.s $(CEFIX) | $(BUILD)
	$(CC) -E -x assembler-with-cpp $(ASM_DEFINES) $(ASM_INCLUDES) $< \
		| sed -E -f $(CEFIX) > $(BUILD)/$*.s
	$(AS) $(AS_FLAGS) $(ASM_INCLUDES) -o $@ $(BUILD)/$*.s

$(BUILD)/VDC2.o: $(SRC)/VDC.s $(CEFIX) $(CORE)/mkvdc2.sh | $(BUILD)
	$(CC) -E -x assembler-with-cpp $(ASM_DEFINES) $(ASM_INCLUDES) -I$(SRC) $< > $(BUILD)/VDC2.pre.s
	sh $(CORE)/mkvdc2.sh $(BUILD)/VDC2.pre.s $(CEFIX) > $(BUILD)/VDC2.s
	$(AS) $(AS_FLAGS) $(ASM_INCLUDES) -o $@ $(BUILD)/VDC2.s

$(BUILD)/SgxCE.o: $(CORE)/SgxCE.s | $(BUILD)
	$(CC) -E -x assembler-with-cpp $(ASM_DEFINES) $(ASM_INCLUDES) $< > $(BUILD)/SgxCE.pre.s
	$(AS) $(AS_FLAGS) $(ASM_INCLUDES) -o $@ $(BUILD)/SgxCE.pre.s

$(BUILD)/ce_asmfix.o: $(CORE)/ce_asmfix.s | $(BUILD)
	$(AS) $(AS_FLAGS) -o $@ $<

$(BUILD)/%.o: $(CORE)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/pce_render.o: $(CORE)/pce_render.h

$(BUILD)/pce_core_ce.o: $(CORE)/pce_core_ce.c $(CORE)/pce_core_ce.h $(CORE)/pce_render.h $(CORE)/pce_cd_ce.h include/libretro.h | $(BUILD)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

$(BUILD)/pce_cd_ce.o: $(CORE)/pce_cd_ce.c $(CORE)/pce_cd_ce.h | $(BUILD)
	$(CC) $(APP_CFLAGS) -I$(SRC) -c -o $@ $<

$(BUILD)/CUEParser.o: $(SRC)/cueparser/CUEParser.c $(SRC)/cueparser/CUEParser.h compat/nds.h | $(BUILD)
	$(CC) $(CFLAGS) -Icompat -c -o $@ $<

$(APP_BUILD):
	mkdir -p $@

$(APP_BUILD)/%.o: app/%.c $(wildcard app/*.h) $(CORE)/pce_core_ce.h | $(APP_BUILD)
	$(CC) $(APP_CFLAGS) -c -o $@ $<

# ce_res.rc embeds the app icon and the main-menu mascot bitmap (both in
# icon/), so the resource object is rebuilt when either changes.
$(APP_BUILD)/ce_res.o: app/ce_res.rc app/ce_resource.h app/ce_app_config.h icon/poppce.ico icon/poppce_mascot.bmp | $(APP_BUILD)
	$(WINDRES) -F pe-arm-wince-little -Iapp -o $@ $<

$(TARGET): $(CORE_OBJS) $(CE_CORE_OBJS) $(SGX_OBJS) $(CE_OBJS) $(APP_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

strip: $(TARGET)
	$(STRIP) $<

# Symbols the core asm needs from outside itself (C side / libnds),
# i.e. what ce_stubs.c currently has to provide.
undefined: $(CORE_OBJS) $(CE_CORE_OBJS) $(SGX_OBJS) $(BUILD)/ce_asmfix.o $(BUILD)/pce_render.o
	@$(NM) -g --defined-only $^ | awk 'NF==3{print $$3}' | sort -u > $(BUILD)/def.txt
	@$(NM) -u $^ | awk 'NF==2{print $$2}' | sort -u > $(BUILD)/und.txt
	@comm -23 $(BUILD)/und.txt $(BUILD)/def.txt

clean:
	rm -rf $(BUILD) AppMain.exe AppMain_shinonome.exe AppMain_galmuri11.exe
