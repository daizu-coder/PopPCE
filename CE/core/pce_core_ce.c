// SPDX-License-Identifier: MIT
// Copyright (c) 2026 daizu-coder
/*
 * pce_core_ce.c - libretro-shaped adapter over the NitroGrafx PC Engine
 * core, so the CE frontend (CE/app/, written against the libretro API)
 * can drive it unchanged.
 *
 * NitroGrafx is not a libretro core: this file plays the part of the DS
 * build's Main.c / FileHandling.c / PCEngine.c for the calls the
 * frontend makes:
 *   retro_init        machineInit() (gfxInit, soundInit, cdInit)
 *   retro_load_game   read the HuCard image into ROM_Space, loadCart()
 *   retro_run         pad -> EMUinput, run() (one frame; the picture
 *                     is drawn line by line into pceFrame by
 *                     CE/core/pce_render.c), pceFrame -> video callback
 *                     PSG -> audio callback (Sound.s:soundRender drains
 *                     the per-scanline PSG ring once a frame)
 *   retro_serialize   same layout as PCEngine.c:pcePackState (plus the
 *                     SuperGrafx parts at the end for SGX games)
 *
 * SuperGrafx: a .sgx file, or one of the SGX HuCards Mednafen knows by
 * CRC32, turns on CE/core/SgxCE.s:ceSgxOn (second VDC, VPC) and maps the
 * extra 24KB of RAM (banks $F9-$FB).
 *
 * CD-ROM: a .cue is loaded like FileHandling.c:selectCDROM does it - the
 * System Card (pceCoreSetBiosPathW) goes in as the HuCard, and the disc
 * (CE/core/pce_cd_ce.c) is inserted. BRAM (the first 2KB of pceSRAM) is
 * the save RAM for CD games. Not the Arcade Card's RAM in save states.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <libretro.h>

#include "pce_core_ce.h"
#include "pce_render.h"
#include "pce_cd_ce.h"
#include "ce_log.h"

/* --- core (asm), see the headers in source/ ---------------------------------------- */
extern void machineInit(void);
extern void loadCart(void);
extern void run(void);
extern void mirrorBytes(unsigned char *memory, int length);
extern unsigned char ROM_Space[0x290000];
extern unsigned int g_ROM_Size;
extern unsigned int EMUinput;
extern unsigned int joyCfg;			/* io.s, bit 27 = 6-button pad */
extern unsigned char pceSRAM[0x2000];
extern unsigned char pceRAM[0x2000];
extern unsigned char pceVRAM[0x10000];

/* Save state pieces (source/ARMH6280/H6280.h, VDC.h, VCE.h,
 * PCEPSG/pcepsg.h). The C structs in those headers do not match the asm
 * layouts, so the state blocks are only passed through
 * as opaque pointers here. */
extern unsigned char h6280OpTable[];
extern unsigned char vdcState[];
extern unsigned char vceState[];
extern unsigned char PSG_0[];
extern int h6280SaveState(void *destination, const void *cpu);
extern int h6280LoadState(void *cpu, const void *source);
extern int h6280GetStateSize(void);
extern int vdcSaveState(void *destination, const void *chip);
extern int vdcLoadState(void *chip, const void *source);
extern int vdcGetStateSize(void);
extern int vceSaveState(void *destination, const void *chip);
extern int vceLoadState(void *chip, const void *source);
extern int vceGetStateSize(void);
extern int pcePSGSaveState(void *destination, const void *chip);
extern int pcePSGLoadState(void *chip, const void *source);
extern int pcePSGGetStateSize(void);

extern unsigned char powerIsOn;		/* CE/core/ce_stubs.c */
extern unsigned char gDebugSet;		/* CE/core/ce_stubs.c: CD command log on */

/* SuperGrafx (CE/core/SgxCE.s, VDC2 = VDC.s copied by CE/core/mkvdc2.sh) */
extern unsigned char ceSgxOn;
extern unsigned char vpcRegs[8];
extern unsigned char sgxRAM[0x8000];		/* Cart.s, pceRAM is its first 8KB */
extern unsigned char pceVRAM2[0x10000];
extern unsigned char vdcState_2[];
extern int vdcSaveState_2(void *destination, const void *chip);
extern int vdcLoadState_2(void *chip, const void *source);
/* Cart.s per-bank tables: base address, read and write handler
 * (RDMEMTBL_/WRMEMTBL_ made global by CE/core/cefix.sed) */
extern void *MEMMAPTBL_[256];
extern void *RDMEMTBL_[256];
extern void *WRMEMTBL_[256];
extern void mem_R(void);			/* Memory.s: read through the bank's base */
extern void xram_W(void);			/* Memory.s: write through the bank's base */
#define SGX_EXTRA_RAM	(sizeof(sgxRAM) - 0x2000)

/* Sound.s: mixes `length` stereo samples (16-bit L in the low half, R in
 * the high half of each word) from the PSG into dest. */
extern void soundRender(int length, void *dest);

/* DS key bits io.s:refreshEMUjoypads reads from EMUinput */
#define KEY_A		0x001	/* PCE button I */
#define KEY_B		0x002	/* PCE button II */
#define KEY_SELECT	0x004
#define KEY_START	0x008	/* PCE Run */
#define KEY_RIGHT	0x010
#define KEY_LEFT	0x020
#define KEY_UP		0x040
#define KEY_DOWN	0x080
/* Extra 6-button pad buttons. io.s sends EMUinput bits 8-11 as the pad's
 * third nibble (III, IV, V, VI in bits 0-3), but first swaps the DS X
 * and Y bits (0x400/0x800) - so V and VI are given here pre-swapped. */
#define KEY_III		0x100
#define KEY_IV		0x200
#define KEY_V		0x800
#define KEY_VI		0x400
#define JOYCFG_6BUTTON	0x08000000
/* Largest plain HuCard. Anything bigger uses the Street Fighter II'
 * mapper (Memory.s:romWrite switches banks only when romMask >= 0x80,
 * i.e. more than 1MB), and SF2' is the only 6-button HuCard. */
#define PCE_PLAIN_HUCARD_MAX	0x100000
/* 2-button pad turbo (VI = turbo I, V = turbo II): pressed for
 * TURBO_HALF frames, released for TURBO_HALF frames (5 Hz). */
#define TURBO_HALF		6

/* PSG output rate. The mixer's pitch is fixed by PSGDIVIDE (patched to 81
 * in CE/core/cefix.sed, i.e. tuned for 44100 Hz), so this is the rate the
 * samples must be played at. 44100 is also one of the rates this device
 * plays reliably, so the frontend's "native rate" setting opens it as is. */
#define PCE_AUDIO_RATE	44100
/* NTSC PC Engine: 7.16 MHz / (455 * 263) */
#define PCE_FPS			59.826
#define PCE_FPS_X1000	59826
/* Upper bound of samples per frame (44100 / 59.826 = 737.1) */
#define PCE_AUDIO_MAX_FRAME	800

/* PSG register offsets in PSG_0 (PCEPSG/pcepsg.i) */
#define PSG_AMPLITUDE_CHG	326

static retro_environment_t environ_cb;
static retro_video_refresh_t video_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static WCHAR romPathW[MAX_PATH];
static WCHAR biosPathW[MAX_PATH];
static int gameLoaded;
static int isSgx;
static int isCd;

extern unsigned char gHwFlags;		/* Cart.s */
#define HW_AC_CARD	0x10			/* Equates.h:AC_CARD */

/* A CD game on a System Card that enabled the Arcade Card (Cart.s:
 * checkMachine; with a 256KB card, every CD game) */
static int hasAc(void)
{
	return isCd && (gHwFlags & HW_AC_CARD);
}

static size_t stateUsed;			/* see pceCoreStateUsedSize */
static int padSixButtons;
static unsigned lastFrameCount;

/* Frame skip (Video Config "Frame Skip" > 0): the frontend asks for the
 * core option below and, when it is "auto", reports its audio buffer
 * state through the callback every frame. A frame is skipped (not drawn,
 * still emulated) while the buffer is about to run dry. */
#define PCE_OPT_FRAMESKIP	"pce_frameskip"
static int frameskipAuto;
static int audioBuffActive;
static int audioBuffUnderrun;

/* ----------------------------------------------------------------------- */
void pceCoreSetRomPathW(const wchar_t *path)
{
	wcsncpy(romPathW, path ? path : L"", MAX_PATH - 1);
	romPathW[MAX_PATH - 1] = 0;
}

void pceCoreSetBiosPathW(const wchar_t *path)
{
	wcsncpy(biosPathW, path ? path : L"", MAX_PATH - 1);
	biosPathW[MAX_PATH - 1] = 0;
}

unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

void retro_set_environment(retro_environment_t cb) { environ_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { (void)cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_controller_port_device(unsigned port, unsigned device)
{
	(void)port;
	(void)device;
}

void retro_get_system_info(struct retro_system_info *info)
{
	memset(info, 0, sizeof(*info));
	info->library_name = "NitroGrafx (CE port)";
	info->library_version = "CE";
	info->valid_extensions = "pce|sgx|cue";
	info->need_fullpath = true;		/* read here, see loadRomFile() */
	info->block_extract = false;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
	memset(info, 0, sizeof(*info));
	info->geometry.base_width = pceFrameWidth;
	info->geometry.base_height = pceFrameHeight;
	info->geometry.max_width = PCE_FB_PITCH;
	info->geometry.max_height = PCE_FB_HEIGHT;
	info->geometry.aspect_ratio = 4.0f / 3.0f;
	info->timing.fps = PCE_FPS;
	info->timing.sample_rate = PCE_AUDIO_RATE;
}

static void RETRO_CALLCONV audioBuffStatus(bool active, unsigned occupancy, bool underrun_likely)
{
	(void)occupancy;
	audioBuffActive = active;
	audioBuffUnderrun = underrun_likely;
}

static void checkVariables(void)
{
	struct retro_variable var = { PCE_OPT_FRAMESKIP, NULL };
	int wantAuto = 0;
	struct retro_audio_buffer_status_callback cb = { audioBuffStatus };

	if (environ_cb && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
		wantAuto = strcmp(var.value, "auto") == 0;
	if (wantAuto == frameskipAuto)
		return;
	frameskipAuto = wantAuto;
	audioBuffActive = 0;
	audioBuffUnderrun = 0;
	environ_cb(RETRO_ENVIRONMENT_SET_AUDIO_BUFFER_STATUS_CALLBACK, wantAuto ? &cb : NULL);
}

/* updateAmplitudes only runs when amplitudeChg is set, and the volumes it
 * computes live in the mixer code, not in PSG_0. After a reset or a state
 * load the patched code no longer matches PSG_0, so recompute them all
 * on the next mix. */
static void psgRefreshVolumes(void)
{
	PSG_0[PSG_AMPLITUDE_CHG] = 0x3F;
}

void retro_init(void)
{
	machineInit();
}

static uint32_t crc32(const unsigned char *p, size_t n);

/* CD games that support the 6-button pad (the list on the "PC Engine
 * Kyou no Uta" controller page), by the CRC32 of the disc's IPL sector
 * (see pceCdIpl). The IPL's name field alone is not enough: several
 * games leave "SAMPLE PROGRAM" there. Only CRCs read from real images
 * are listed. */
static const uint32_t sixButtonCdIpls[] = {
	0x17cd1516,		/* Advanced V.G. */
	0x42eab244,		/* Emerald Dragon (Rev 1) */
	0x74679c7b,		/* Kakutou Haou Densetsu Algunos */
	0xf2465fc0,		/* Garou Densetsu 2 */
	0x40804bb7,		/* Garou Densetsu Special */
	0xa3c3c584,		/* Kabuki Ittouryoudan */
	0x66749ec0,		/* Ginga Ojousama Densetsu Yuna 2 */
	0x26e3df92,		/* Super Real Mahjong PII-III Custom */
	0xc70eb259,		/* Super Real Mahjong P.V Custom */
	0x7dd93275,		/* Doukyuusei (Rev 4) */
	0x6363e49b,		/* Fire Pro Joshi - Dome Choujo Taisen */
	0x847a7574,		/* Flash Hiders */
	0xfdbf5806,		/* Princess Maker 2 */
	0xe59c3e5f,		/* Martial Champion */
	0x52840ae8,		/* Mahjong Sword - Princess Quest Gaiden */
	0x7ce39f84,		/* Ryuuko no Ken */
	0xe0d1b1db,		/* Linda Cube (Linda3, Rev 2) */
	0xa0d5bd7e,		/* World Heroes 2 */
};

static int isSixButtonCd(void)
{
	const unsigned char *ipl = pceCdIpl();
	uint32_t crc;
	unsigned i;

	if (!ipl)
		return 0;
	crc = crc32(ipl, PCE_CD_IPL_SIZE);
	CeLog("pce cd: IPL crc 0x%08x", (unsigned)crc);
	for (i = 0; i < sizeof(sixButtonCdIpls) / sizeof(sixButtonCdIpls[0]); i++)
		if (crc == sixButtonCdIpls[i])
			return 1;
	return 0;
}

/* A 6-button pad answers every other read with III-VI, which 2-button
 * games take as all four directions held. So the pad is a plain 2-button
 * one unless the game is known to support 6 buttons. */
static void selectPadType(size_t romSize)
{
	int six = isCd ? isSixButtonCd() : romSize > PCE_PLAIN_HUCARD_MAX;

	padSixButtons = six;
	if (six)
		joyCfg |= JOYCFG_6BUTTON;
	else
		joyCfg &= ~JOYCFG_6BUTTON;
	CeLog("pce: %u byte %s, %d-button pad", (unsigned)romSize,
		isCd ? "System Card" : "HuCard", six ? 6 : 2);
}

void retro_deinit(void)
{
}

/* ----------------------------------------------------------------------- */
static const void *findBytes(const void *hay, size_t hayLen, const void *needle, size_t len)
{
	const unsigned char *h = hay;
	size_t i;

	for (i = 0; i + len <= hayLen; i++)
		if (memcmp(h + i, needle, len) == 0)
			return h + i;
	return NULL;
}

/* Same test as FileHandling.c:isEncryptedRom - US (TG-16) dumps named
 * "(U)"/"(USA)" whose data is bit-reversed. An unscrambled image carries
 * " NEC " near the start; a scrambled one has the scrambled bytes. */
static int isEncryptedRom(const unsigned char *rom, size_t size, const WCHAR *name)
{
	static const unsigned char nec[5] = { ' ', 'N', 'E', 'C', ' ' };
	static const unsigned char key[5] = { 0x04, 0x72, 0xA2, 0xC2, 0x04 };
	static const size_t ranges[] = { 0x20, 0x80, 0x2000 };
	int i;

	if (!name || (!wcsstr(name, L"(U)") && !wcsstr(name, L"(USA)")))
		return 0;
	for (i = 0; i < 3; i++) {
		size_t len = ranges[i] < size ? ranges[i] : size;
		if (i < 2 && findBytes(rom, len, nec, sizeof(nec)))
			return 0;
		if (findBytes(rom, len, key, sizeof(key)))
			return 1;
	}
	return 0;
}

/* Reads a file of at most maxSize bytes into dest; returns its size or 0. */
static size_t readFileW(const WCHAR *path, void *dest, size_t maxSize)
{
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD got = 0, fileSize;
	size_t size = 0;

	if (h == INVALID_HANDLE_VALUE)
		return 0;
	fileSize = GetFileSize(h, NULL);
	if (fileSize != 0 && fileSize <= maxSize
		&& ReadFile(h, dest, fileSize, &got, NULL) && got == fileSize)
		size = fileSize;
	CloseHandle(h);
	return size;
}

/* Reads the image into ROM_Space; returns its size or 0. */
static size_t loadRomFile(const struct retro_game_info *game)
{
	size_t size = 0;

	if (romPathW[0]) {
		size = readFileW(romPathW, ROM_Space, sizeof(ROM_Space));
	} else if (game && game->data && game->size <= sizeof(ROM_Space)) {
		memcpy(ROM_Space, game->data, game->size);
		size = game->size;
	} else if (game && game->path) {
		FILE *f = fopen(game->path, "rb");
		if (!f)
			return 0;
		size = fread(ROM_Space, 1, sizeof(ROM_Space), f);
		fclose(f);
	}
	return size;
}

/* --- SuperGrafx --------------------------------------------------------- */
/* SuperGrafx HuCards that are usually dumped as .pce (CRC32 of the image
 * without a copier header). Factual data, taken as a reference from the
 * SuperGrafx game list in beetle-supergrafx-libretro (no Mednafen code is
 * used; see CE/THIRDPARTY_LICENSES.txt). */
static const uint32_t sgxCrcs[] = {
	0xbebfe042,		/* Darius Plus */
	0x4c2126b0,		/* Aldynes */
	0x8c4588e2,		/* 1941 - Counter Attack */
	0x1f041166,		/* Madouou Granzort */
	0xb486a8ed,		/* Daimakaimura */
	0x3b13af61,		/* Battle Ace */
};

static uint32_t crc32(const unsigned char *p, size_t n)
{
	uint32_t c = 0xFFFFFFFFu;
	int k;

	while (n--) {
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
	}
	return ~c;
}

static int hasSgxExtension(const WCHAR *pathW, const char *path)
{
	if (pathW && pathW[0]) {
		const WCHAR *dot = wcsrchr(pathW, L'.');
		return dot && (dot[1] | 0x20) == 's' && (dot[2] | 0x20) == 'g'
			&& (dot[3] | 0x20) == 'x' && dot[4] == 0;
	}
	if (path) {
		const char *dot = strrchr(path, '.');
		return dot && (dot[1] | 0x20) == 's' && (dot[2] | 0x20) == 'g'
			&& (dot[3] | 0x20) == 'x' && dot[4] == 0;
	}
	return 0;
}

static int isSgxGame(const unsigned char *rom, size_t size, const WCHAR *pathW, const char *path)
{
	uint32_t crc;
	size_t i;

	if (hasSgxExtension(pathW, path))
		return 1;
	crc = crc32(rom, size);
	for (i = 0; i < sizeof(sgxCrcs) / sizeof(sgxCrcs[0]); i++)
		if (crc == sgxCrcs[i])
			return 1;
	return 0;
}

/* Cart.s:loadCart maps all of $F8-$FB to the 8KB pceRAM (ram_R/ram_W,
 * which only reach it through the zero page register). On a SuperGrafx
 * $F9-$FB are the next 24KB of sgxRAM, read and written through the
 * bank base like the CD RAM. */
static void mapSgxRam(void)
{
	int bank;

	memset(sgxRAM + 0x2000, 0, SGX_EXTRA_RAM);
	for (bank = 0xF9; bank <= 0xFB; bank++) {
		MEMMAPTBL_[bank] = sgxRAM + (bank - 0xF8) * 0x2000;
		RDMEMTBL_[bank] = (void *)mem_R;
		WRMEMTBL_[bank] = (void *)xram_W;
	}
}

/* --- CD-ROM ---------------------------------------------------------------- */
#define SYSCARD_SIZE	0x40000		/* Cart.s:biosSpace, 256KB */
#define BRAM_SIZE		0x800		/* the used part of pceSRAM */

static int hasExtensionW(const WCHAR *path, const WCHAR *ext)
{
	const WCHAR *dot = path ? wcsrchr(path, L'.') : NULL;
	return dot && _wcsicmp(dot, ext) == 0;
}

int pceCoreIsCdImagePath(const wchar_t *path)
{
	return hasExtensionW(path, L".cue");
}

static unsigned char bitReverse(unsigned char b)
{
	b = (unsigned char)((b >> 4) | (b << 4));
	b = (unsigned char)(((b & 0xCC) >> 2) | ((b & 0x33) << 2));
	return (unsigned char)(((b & 0xAA) >> 1) | ((b & 0x55) << 1));
}

/* Every System Card ends with "PC Engine CD-ROM SYSTEM" and the Hudson /
 * NEC copyright (System Card 3.0: at 0x3FFB6). US dumps are bit-reversed
 * like US HuCards. Returns 0 if not found, 1 if found as is, 2 if found
 * bit-reversed. */
static int findSyscardSignature(const unsigned char *rom, size_t size)
{
	static const char sig[] = "CD-ROM SYSTEM";
	unsigned char rev[0x200];
	size_t tail = size < sizeof(rev) ? size : sizeof(rev), i;
	const unsigned char *end = rom + size - tail;

	if (findBytes(end, tail, sig, sizeof(sig) - 1))
		return 1;
	for (i = 0; i < tail; i++)
		rev[i] = bitReverse(end[i]);
	return findBytes(rev, tail, sig, sizeof(sig) - 1) ? 2 : 0;
}

/* Known Super System Card 3.0 dumps (CRC32 without a copier header),
 * preferred when a folder holds several System Cards. */
static const uint32_t syscard3Crcs[] = {
	0x6d9a73ef,		/* Super CD-ROM2 System 3.00 (J) */
	0x2b5b75fe,		/* TurboGrafx CD Super System Card 3.0 (U) */
};

int pceCoreProbeBiosW(const wchar_t *path)
{
	static unsigned char buf[SYSCARD_SIZE + 0x200];
	unsigned char tail[0x200];
	const unsigned char *rom = buf;
	HANDLE h;
	DWORD fileSize, got = 0;
	size_t i;
	uint32_t crc;

	/* Size and signature first (the last 512 bytes only), so a folder of
	 * 256KB HuCards is not read in full. */
	h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (h == INVALID_HANDLE_VALUE)
		return 0;
	fileSize = GetFileSize(h, NULL);
	if ((fileSize == SYSCARD_SIZE || fileSize == SYSCARD_SIZE + 0x200)
		&& SetFilePointer(h, fileSize - sizeof(tail), NULL, FILE_BEGIN) != 0xFFFFFFFF)
		ReadFile(h, tail, sizeof(tail), &got, NULL);
	CloseHandle(h);
	if (got != sizeof(tail) || !findSyscardSignature(tail, sizeof(tail)))
		return 0;

	if (readFileW(path, buf, sizeof(buf)) != fileSize)
		return 0;
	if (fileSize == SYSCARD_SIZE + 0x200)
		rom += 0x200;			/* copier header */
	crc = crc32(rom, SYSCARD_SIZE);
	for (i = 0; i < sizeof(syscard3Crcs) / sizeof(syscard3Crcs[0]); i++)
		if (crc == syscard3Crcs[i])
			return 2;
	return 1;
}

/* Loads biosPathW into ROM_Space (Cart.s: biosSpace is the same memory)
 * and opens the disc. Returns the System Card's size, or 0. */
static size_t loadCdGame(void)
{
	size_t size;

	if (!biosPathW[0]) {
		CeLog("pce cd: no System Card set");
		return 0;
	}
	size = readFileW(biosPathW, ROM_Space, SYSCARD_SIZE + 0x200);
	if (size == SYSCARD_SIZE + 0x200) {
		size = SYSCARD_SIZE;
		memmove(ROM_Space, ROM_Space + 0x200, size);
	}
	if (size != SYSCARD_SIZE) {
		CeLog("pce cd: System Card not readable or not 256KB");
		return 0;
	}
	switch (findSyscardSignature(ROM_Space, size)) {
	case 0:
		CeLog("pce cd: not a System Card");
		return 0;
	case 2:
		mirrorBytes(ROM_Space, (int)size);	/* US dump */
		break;
	}
	if (!pceCdOpenCue(romPathW))
		return 0;
	return size;
}

/* loadCart plus what this port adds on top (also a reset) */
static void startCart(void)
{
	ceSgxOn = (unsigned char)isSgx;		/* before loadCart: gfxReset resets VDC2 */
	loadCart();
	if (isSgx)
		mapSgxRam();
	psgRefreshVolumes();
}

bool retro_load_game(const struct retro_game_info *game)
{
	enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
	size_t size;

	if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
		return false;

	pceCdClose();
	isCd = romPathW[0] && pceCoreIsCdImagePath(romPathW);
	size = isCd ? loadCdGame() : loadRomFile(game);
	if (size == 0) {
		pceCdClose();
		romPathW[0] = 0;
		isCd = 0;
		return false;
	}

	if (isCd) {
		isSgx = 0;			/* SuperGrafx + CD (HW_SGX_ACPRO) is not handled */
	} else {
		/* Same as FileHandling.c:loadPCEROM: drop a 512 byte copier
		 * header, descramble US images. */
		if ((size & 0x3FF) == 0x200) {
			size -= 0x200;
			memmove(ROM_Space, ROM_Space + 0x200, size);
		}
		if (isEncryptedRom(ROM_Space, size, romPathW[0] ? romPathW : NULL))
			mirrorBytes(ROM_Space, (int)size);
		isSgx = isSgxGame(ROM_Space, size, romPathW, game ? game->path : NULL);
	}
	g_ROM_Size = size;
	romPathW[0] = 0;		/* one-shot, see pceCoreSetRomPathW() */
	if (isSgx)
		CeLog("pce: SuperGrafx");

	/* Each CD game starts from a fresh BRAM (Cart.s:checkPCEBRAM formats
	 * it in loadCart) and the frontend then loads its own .srm, so one
	 * game's saves do not show up in the next. */
	if (isCd)
		memset(pceSRAM, 0, sizeof(pceSRAM));
	selectPadType(size);
	startCart();
	checkVariables();
	powerIsOn = 1;
	gameLoaded = 1;
	lastFrameCount = pceFrameCount;
	return true;
}

bool retro_load_game_special(unsigned type, const struct retro_game_info *info, size_t num)
{
	(void)type;
	(void)info;
	(void)num;
	return false;
}

void retro_unload_game(void)
{
	gameLoaded = 0;
	powerIsOn = 0;
	pceCdClose();
	isCd = 0;
}

void retro_reset(void)
{
	if (gameLoaded)
		startCart();		/* Gui.c:resetGame does loadCart too */
}

unsigned retro_get_region(void)
{
	return RETRO_REGION_NTSC;
}

/* ----------------------------------------------------------------------- */
static unsigned readJoypad(void)
{
	static const struct { unsigned id, bit; } map[] = {
		{ RETRO_DEVICE_ID_JOYPAD_UP,     KEY_UP },
		{ RETRO_DEVICE_ID_JOYPAD_DOWN,   KEY_DOWN },
		{ RETRO_DEVICE_ID_JOYPAD_LEFT,   KEY_LEFT },
		{ RETRO_DEVICE_ID_JOYPAD_RIGHT,  KEY_RIGHT },
		{ RETRO_DEVICE_ID_JOYPAD_A,      KEY_A },		/* I */
		{ RETRO_DEVICE_ID_JOYPAD_B,      KEY_B },		/* II */
		{ RETRO_DEVICE_ID_JOYPAD_X,      KEY_III },
		{ RETRO_DEVICE_ID_JOYPAD_Y,      KEY_IV },
		{ RETRO_DEVICE_ID_JOYPAD_L,      KEY_V },
		{ RETRO_DEVICE_ID_JOYPAD_R,      KEY_VI },
		{ RETRO_DEVICE_ID_JOYPAD_SELECT, KEY_SELECT },
		{ RETRO_DEVICE_ID_JOYPAD_START,  KEY_START },	/* Run */
	};
	unsigned keys = 0, i;

	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, map[i].id))
			keys |= map[i].bit;

	/* A 2-button pad has no III-VI: VI and V become turbo I and II. */
	if (!padSixButtons) {
		static unsigned turboFrame;
		int on = (turboFrame++ / TURBO_HALF) & 1;

		if (on && (keys & KEY_VI))
			keys |= KEY_A;
		if (on && (keys & KEY_V))
			keys |= KEY_B;
		keys &= ~(KEY_III | KEY_IV | KEY_V | KEY_VI);
	}
	return keys;
}

/* One frame of PSG output. The DS build calls soundRender() on maxmod's
 * stream request; here it is called once per emulated frame for
 * PCE_AUDIO_RATE / PCE_FPS samples (remainder carried, integer only).
 * With SAMPLE_PLAYING (CE/Makefile) the PSG itself is mixed a few samples
 * per scanline into Sound.s's ring (so DDA samples and mid-frame register
 * writes are heard), and soundRender() drains that ring, nudging the
 * per-scanline sample count to match what is drained. */
/* --- perf log (debug logging only) --------------------------------------
 * Splits "perf: retro_run" into emulation and drawing without timing
 * every line: a per-line timer costs more than it measures on this
 * device (measured on the sister PopSG: hundreds of GetTickCount calls
 * per frame added about 12ms). Instead run() is timed once per
 * frame, and while debug logging is on every PERF_PROBE_EVERY-th frame is
 * a probe frame, in turn:
 *   - not drawn at all (the previous picture stays up, as with frame
 *     skip): drawn minus undrawn is the whole cost of drawing, including
 *     what the renderer's memory traffic costs the emulation around it;
 *   - one part of pce_render.c done twice per line (pceRenderProbe: BG,
 *     sprites, output, tile cache scan): the extra time is that part's
 *     own cost with warm caches.
 * If the parts add up to much less than the whole, the difference is
 * cache misses rather than instructions. GetTickCount is ms resolution,
 * so the averages come from many frames. Also counts
 * ce_stubs.c:ceSyncPsgCode calls and cache flushes. */
#define PERF_PROBE_EVERY	8
#define PERF_WINDOW		600		/* frames per log line, about 10s */
#define PERF_KINDS		(1 + PCE_PROBE_COUNT)	/* normal, undrawn, 4 doubled parts */

extern unsigned cePsgSyncCalls, cePsgSyncFlushes;

/* kind: 0 normal drawn frame, PCE_PROBE_NONE+1 .. = see perfKind() */
static struct {
	unsigned frames, ms[PERF_KINDS], n[PERF_KINDS], drainMs;
	unsigned psgCalls0, psgFlushes0;
	unsigned wallMs;		/* whole retro_run, wall time */
} perf;

enum { PK_DRAWN, PK_UNDRAWN, PK_BG, PK_SPR, PK_OUT, PK_SCAN };

/* ms * 10 / n, for one decimal place */
static unsigned tenths(unsigned ms, unsigned n)
{
	return n ? ms * 10 / n : 0;
}

/* Extra time of a doubled-part kind over a normal drawn frame, in tenths */
static int extraTenths(int kind)
{
	if (!perf.n[kind] || !perf.n[PK_DRAWN])
		return 0;
	return (int)tenths(perf.ms[kind], perf.n[kind]) - (int)tenths(perf.ms[PK_DRAWN], perf.n[PK_DRAWN]);
}

/* CPU time of the threads at the start of the window (see perfCpuLine) */
static unsigned cpuMain0, cpuReader0, cpuLog0;
static void *cpuReaderThread, *cpuLogThread;	/* whose times the 0s are */
static int cpuStarted;

/* Where the frame time goes besides this thread: the frame loop's own CPU
 * time per frame against retro_run's wall time per frame (a gap means
 * another thread held the loop off), and the CPU time of the CD
 * read-ahead thread and the log writer thread. */
static void perfCpuLine(void)
{
	unsigned cMain = CeThreadCpuMs(GetCurrentThread());
	unsigned cReader = CeThreadCpuMs(pceCdReaderThread());
	unsigned cLog = CeThreadCpuMs(CeLogThread());

	/* A new game starts a new read-ahead thread (and turning the log off
	 * and on a new writer): count those from 0, not from the old one */
	if (pceCdReaderThread() != cpuReaderThread || cReader < cpuReader0)
		cpuReader0 = 0;
	if (CeLogThread() != cpuLogThread || cLog < cpuLog0)
		cpuLog0 = 0;
	cpuReaderThread = pceCdReaderThread();
	cpuLogThread = CeLogThread();
	unsigned m = tenths(cMain - cpuMain0, perf.frames);
	unsigned w = tenths(perf.wallMs, perf.frames);
	unsigned rd = tenths(cReader - cpuReader0, perf.frames);
	unsigned lg = tenths(cLog - cpuLog0, perf.frames);
	unsigned flushes, flushMax, dropped;

	CeLogTakeStats(&flushes, &flushMax, &dropped);
	if (cpuStarted)	/* the first window started mid-way */
		CeLog("perf: cpu per frame: loop %u.%ums (retro_run wall %u.%ums), cd reader %u.%ums, log writer %u.%ums;"
			" log %u flushes, longest %ums, %u lines dropped",
			m / 10, m % 10, w / 10, w % 10, rd / 10, rd % 10, lg / 10, lg % 10,
			flushes, flushMax, dropped);
	cpuMain0 = cMain;
	cpuReader0 = cReader;
	cpuLog0 = cLog;
	cpuStarted = 1;
}

static void perfFrame(int kind, unsigned runMs)
{
	unsigned d, k, r, calls, flushes;
	int bg, spr, out, scan;

	perf.ms[kind] += runMs;
	perf.n[kind]++;
	if (++perf.frames < PERF_WINDOW)
		return;

	d = tenths(perf.ms[PK_DRAWN], perf.n[PK_DRAWN]);
	k = tenths(perf.ms[PK_UNDRAWN], perf.n[PK_UNDRAWN]);
	r = (perf.n[PK_UNDRAWN] && d > k) ? d - k : 0;
	bg = extraTenths(PK_BG);
	spr = extraTenths(PK_SPR);
	out = extraTenths(PK_OUT);
	scan = extraTenths(PK_SCAN);
	if (isCd) {
		unsigned w = pceCdWaitMs * 10 / perf.frames;
		CeLog("perf: cd wait %u.%ums/f, longest %ums, %u waits; thread reads %u (%u after a seek), %ums total, longest %ums",
			w / 10, w % 10, pceCdWaitMaxMs, pceCdWaits,
			pceCdReads, pceCdSeeks, pceCdReadMs, pceCdReadMaxMs);
	}
	pceCdReadMs = pceCdReadMaxMs = pceCdReads = pceCdSeeks = 0;
	pceCdWaitMs = pceCdWaitMaxMs = pceCdWaits = 0;
	calls = (cePsgSyncCalls - perf.psgCalls0) * 10 / perf.frames;
	flushes = (cePsgSyncFlushes - perf.psgFlushes0) * 10 / perf.frames;
	CeLog("perf: core run drawn=%u.%ums (n=%u) undrawn=%u.%ums (n=%u) => draw=%u.%ums;"
		" doubled part adds (tenths of ms): bg=%d spr=%d out=%d scan=%d (n=%u/%u/%u/%u);"
		" audio drain=%u.%ums; psg sync %u.%u calls/f, %u.%u flushes/f",
		d / 10, d % 10, perf.n[PK_DRAWN], k / 10, k % 10, perf.n[PK_UNDRAWN], r / 10, r % 10,
		bg, spr, out, scan, perf.n[PK_BG], perf.n[PK_SPR], perf.n[PK_OUT], perf.n[PK_SCAN],
		tenths(perf.drainMs, perf.frames) / 10, tenths(perf.drainMs, perf.frames) % 10,
		calls / 10, calls % 10, flushes / 10, flushes % 10);
	perfCpuLine();
	memset(&perf, 0, sizeof(perf));
	perf.psgCalls0 = cePsgSyncCalls;
	perf.psgFlushes0 = cePsgSyncFlushes;
}

static void sendAudio(void)
{
	static uint32_t mix[PCE_AUDIO_MAX_FRAME];
	static unsigned frac;
	unsigned frames;

	frac += PCE_AUDIO_RATE * 1000u;
	frames = frac / PCE_FPS_X1000;
	frac %= PCE_FPS_X1000;
	if (frames > PCE_AUDIO_MAX_FRAME)
		frames = PCE_AUDIO_MAX_FRAME;

	if (CeLogIsEnabled()) {
		DWORD t0 = GetTickCount();
		soundRender((int)frames, mix);
		perf.drainMs += GetTickCount() - t0;
	} else {
		soundRender((int)frames, mix);
	}
	if (audio_batch_cb)
		audio_batch_cb((const int16_t *)mix, frames);
}

static unsigned scrollLogFrame;

/* PSG state for the debug log: per channel the registers (freq, $804
 * control, $805 balance) and the L/R volumes the mixer really uses (the
 * immediates updateAmplitudes patched into PCEPSGMixer), then global
 * balance, noise ch4/ch5 and LFO. Offsets from PCEPSG/pcepsg.i. */
#define PSG_CH_REGS		288
#define PSG_GLOBAL		321
extern const unsigned char vol0_L[], vol0_R[], vol1_L[], vol1_R[], vol2_L[], vol2_R[],
	vol3_L[], vol3_R[], vol4_L[], vol4_R[], vol5_L[], vol5_R[];

static void logPsgState(void)
{
	static const unsigned char *const vol[12] = {
		vol0_L, vol0_R, vol1_L, vol1_R, vol2_L, vol2_R,
		vol3_L, vol3_R, vol4_L, vol4_R, vol5_L, vol5_R
	};
	char line[400];
	int i, n = 0;

	for (i = 0; i < 6; i++) {
		const unsigned char *r = PSG_0 + PSG_CH_REGS + i * 4;
		n += sprintf(line + n, "%d:f%03x c%02x b%02x v%u/%u ", i,
			(r[0] | r[1] << 8) & 0xFFF, r[2], r[3], *vol[i * 2], *vol[i * 2 + 1]);
	}
	sprintf(line + n, "g%02x n4:%02x n5:%02x lfo%02x/%02x",
		PSG_0[PSG_GLOBAL], PSG_0[PSG_GLOBAL + 1], PSG_0[PSG_GLOBAL + 2],
		PSG_0[PSG_GLOBAL + 3], PSG_0[PSG_GLOBAL + 4]);
	CeLog("psg: %s", line);
}

void retro_run(void)
{
	DWORD tRun = GetTickCount();

	if (!gameLoaded)
		return;

	{
		bool updated = false;
		if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
			checkVariables();
	}
	pceSkipRequest = frameskipAuto && audioBuffActive && audioBuffUnderrun;

	input_poll_cb();
	EMUinput = readJoypad();
	gDebugSet = (unsigned char)(isCd && CeLogIsEnabled());	/* pce_cd_ce.c:debugOutput */

	if (CeLogIsEnabled()) {
		static unsigned frameNo, probeNo;
		unsigned before = pceFrameCount;
		int kind = PK_DRAWN;
		DWORD t0;

		pceRenderProbe = PCE_PROBE_NONE;
		if (!pceSkipRequest && ++frameNo >= PERF_PROBE_EVERY) {
			frameNo = 0;
			/* undrawn, then each doubled part in turn */
			switch (probeNo++ % PCE_PROBE_COUNT) {
			case 0: pceSkipRequest = 1; break;
			case 1: pceRenderProbe = PCE_PROBE_BG; kind = PK_BG; break;
			case 2: pceRenderProbe = PCE_PROBE_SPR; kind = PK_SPR; break;
			case 3: pceRenderProbe = PCE_PROBE_OUT; kind = PK_OUT; break;
			default: pceRenderProbe = PCE_PROBE_SCAN; kind = PK_SCAN; break;
			}
		}
		/* every ~2 s: the BG scroll of each line (raster splits) */
		if (++scrollLogFrame >= 120) {
			scrollLogFrame = 0;
			pceScrollLogRequest = 1;
			logPsgState();
		}
		t0 = GetTickCount();
		run();
		if (!pceScrollLogRequest && pceScrollLog[0]) {
			const char *q = pceScrollLog;
			size_t n = strlen(q);
			for (; n > 0; q += 400, n = n > 400 ? n - 400 : 0)	/* CeLog lines are < 512 */
				CeLog("pce scroll: %.400s", q);
			pceScrollLog[0] = 0;
		}
		if (pceFrameCount == before)
			kind = PK_UNDRAWN;		/* probe or frame skip */
		perfFrame(kind, (unsigned)(GetTickCount() - t0));
		pceRenderProbe = PCE_PROBE_NONE;
		if (isCd)
			pceCdDebugFrame();
	} else {
		run();
	}

	if (pceFrameCount != lastFrameCount) {
		lastFrameCount = pceFrameCount;
		video_cb(pceFrame, pceFrameWidth, pceFrameHeight, PCE_FB_PITCH * sizeof(uint16_t));
	} else {
		video_cb(NULL, pceFrameWidth, pceFrameHeight, PCE_FB_PITCH * sizeof(uint16_t));
	}
	sendAudio();
	if (CeLogIsEnabled())
		perf.wallMs += GetTickCount() - tRun;
}

/* --- save states (layout of PCEngine.c:pcePackState) ------------------- */
/* SuperGrafx games append: the other 24KB of RAM, VDC2's VRAM and
 * registers (same state block as VDC1), and the VPC registers. */
/* Without the Arcade Card block, which is the only part that varies */
static size_t stateBaseSize(void)
{
	size_t size = sizeof(pceSRAM) + sizeof(pceRAM) + sizeof(pceVRAM)
		+ h6280GetStateSize() + vdcGetStateSize() + vceGetStateSize()
		+ pcePSGGetStateSize();

	if (isSgx)
		size += SGX_EXTRA_RAM + sizeof(pceVRAM2) + vdcGetStateSize() + sizeof(vpcRegs);
	if (isCd)
		size += pceCdStateSize();
	return size;
}

size_t retro_serialize_size(void)
{
	return stateBaseSize() + (hasAc() ? pceAcStateMaxSize() : 0);
}

size_t pceCoreStateUsedSize(void)
{
	return stateUsed;
}

bool retro_serialize(void *data, size_t size)
{
	unsigned char *p = data;

	if (!gameLoaded || size < retro_serialize_size())
		return false;
	memcpy(p, pceSRAM, sizeof(pceSRAM));
	p += sizeof(pceSRAM);
	memcpy(p, pceRAM, sizeof(pceRAM));
	p += sizeof(pceRAM);
	memcpy(p, pceVRAM, sizeof(pceVRAM));
	p += sizeof(pceVRAM);
	p += h6280SaveState(p, h6280OpTable);
	p += vdcSaveState(p, vdcState);
	p += vceSaveState(p, vceState);
	p += pcePSGSaveState(p, PSG_0);
	if (isSgx) {
		memcpy(p, sgxRAM + 0x2000, SGX_EXTRA_RAM);
		p += SGX_EXTRA_RAM;
		memcpy(p, pceVRAM2, sizeof(pceVRAM2));
		p += sizeof(pceVRAM2);
		p += vdcSaveState_2(p, vdcState_2);
		memcpy(p, vpcRegs, sizeof(vpcRegs));
		p += sizeof(vpcRegs);
	}
	if (isCd) {
		pceCdSaveState(p);
		p += pceCdStateSize();
	}
	if (hasAc())
		p += pceAcSaveState(p);
	stateUsed = (size_t)(p - (unsigned char *)data);
	return true;
}

bool retro_unserialize(const void *data, size_t size)
{
	const unsigned char *p = data;

	size_t base = stateBaseSize();

	/* A CD state from before the Arcade Card block (size == base) still
	 * loads; the card keeps what it has. */
	if (!gameLoaded || size < base)
		return false;
	if (isCd && !pceCdStateValid(p + (base - pceCdStateSize())))
		return false;
	if (size > base && !(hasAc() && pceAcStateValid(p + base, size - base)))
		return false;
	memcpy(pceSRAM, p, sizeof(pceSRAM));
	p += sizeof(pceSRAM);
	memcpy(pceRAM, p, sizeof(pceRAM));
	p += sizeof(pceRAM);
	memcpy(pceVRAM, p, sizeof(pceVRAM));
	p += sizeof(pceVRAM);
	p += h6280LoadState(h6280OpTable, p);
	p += vdcLoadState(vdcState, p);		/* also marks all tiles dirty */
	p += vceLoadState(vceState, p);
	p += pcePSGLoadState(PSG_0, p);
	if (isSgx) {
		memcpy(sgxRAM + 0x2000, p, SGX_EXTRA_RAM);
		p += SGX_EXTRA_RAM;
		memcpy(pceVRAM2, p, sizeof(pceVRAM2));
		p += sizeof(pceVRAM2);
		p += vdcLoadState_2(vdcState_2, p);	/* after the VCE: uses its dot clock */
		memcpy(vpcRegs, p, sizeof(vpcRegs));
		p += sizeof(vpcRegs);
	}
	if (isCd) {
		pceCdLoadState(p);
		p += pceCdStateSize();
	}
	if (size > base)
		pceAcLoadState(p, size - base);
	psgRefreshVolumes();
	return true;
}

/* --- memory ------------------------------------------------------------ */
void *retro_get_memory_data(unsigned id)
{
	/* No battery save for HuCards. CD games keep theirs in the CD-ROM
	 * unit's BRAM. */
	if (id == RETRO_MEMORY_SAVE_RAM)
		return isCd ? pceSRAM : NULL;
	if (id == RETRO_MEMORY_SYSTEM_RAM)
		return pceRAM;
	return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
	if (id == RETRO_MEMORY_SAVE_RAM)
		return isCd ? BRAM_SIZE : 0;
	if (id == RETRO_MEMORY_SYSTEM_RAM)
		return isSgx ? sizeof(sgxRAM) : sizeof(pceRAM);
	return 0;
}

void retro_cheat_reset(void)
{
}

void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
	(void)index;
	(void)enabled;
	(void)code;
}
