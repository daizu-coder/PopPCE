// SPDX-License-Identifier: MIT
// Copyright (c) 2026 daizu-coder
/*
 * pce_cd_ce.c - CD-ROM image access for the NitroGrafx core on CE: the
 * part of the DS build's FileHandling.c that source/cdrom.s and
 * source/Sound.s call (CD_ReadByte, CD_FillBuffer, CD_SeekPos,
 * CD_ResetBuffer, cdBuffer, cdReadPtr, cdIsBinCue), plus opening a
 * .cue/.bin pair and its save state part.
 *
 * Same scope as upstream: one .bin holding every track (a cue sheet
 * with a single FILE; one converted from .chd is like that). The file is read with Win32 calls on a wide path, so
 * Japanese folder and file names work. The cue sheet is parsed by
 * upstream's source/cueparser/CUEParser.c (startParser/nextTrack), and
 * the TOC is built the way FileHandling.c:CD_ConvertCueFile does it.
 *
 * Upstream reads synchronously with fread; here a read-ahead thread
 * does the file reads (see cdRead) so that the device's slow and uneven
 * storage does not stall the frame. Its read times (wall clock, so they include time the thread
 * was preempted) and the time the core spends waiting for it go to
 * pce_core_ce.c's perf log.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <nds.h>
#include "cueparser/CUEParser.h"

#include "pce_cd_ce.h"
#include "ce_log.h"

/* CUEParser.c (not in its header) */
extern void startParser(const char *cueSheet);
extern const CUETrackInfo *nextTrack(uint32_t prevFileSize);

/* cdrom.s: the drive's state block (made global by CE/core/cefix.sed) */
extern unsigned char cdromState[], cdromStateEnd[];
/* Cart.s: ADPCM RAM (64KB), then Super CD-ROM RAM (192KB) and CD-ROM RAM
 * (64KB), back to back */
extern unsigned char CD_PCM_RAM[];

/* cdrom.s (source/cdrom.h, with the room cdrom.s reserves for 99 tracks
 * after the TOC header spelt out) */
typedef struct {
	u8 mode;			/* 0 audio, 4 data 2048, 8 data 2352 */
	u8 LBA0, LBA1, LBA2;
	u32 start;			/* byte offset in the .bin */
} CD_TRACK;
extern struct {
	char magic[8];		/* TGCD0100 */
	u32 padding0;
	u8 trackCount;
	u8 endLBA0, endLBA1, endLBA2;
	CD_TRACK tracks[99];
} cdRomToc;
extern u8 cdInserted;
extern int cdFileSize;
#define CD_RAM_TOTAL	0x50000

#define CUE_MAX_SIZE	0x2000		/* same as CUEParser.c:readCue */

/* --- what the core asm reads (FileHandling.c) ------------------------------ */
int cdReadPtr;
int cdIsBinCue;
char cdBuffer[0x2000];

static HANDLE cdHandle = INVALID_HANDLE_VALUE;
static int cdWritePtr;
static int cdDataLeft;
static int cdDatatrackMode;

/* --- read-ahead thread -----------------------------------------------------
 * On the device a ReadFile of the .bin costs about 2ms per KB and now and
 * then stalls for most of a second, so reads are done by a thread of their
 * own, on a handle of its own, into a 256KB ring (about 1.5s of CD audio)
 * running ahead of where the core reads. The stream is restarted at each
 * seek (CD_SeekPos), so CD audio is prefetched during the drive's seek
 * time that cdrom.s already emulates. cdRead copies from the ring, and
 * waits for the thread only when the bytes are not there yet (right after
 * a data seek, or when the thread falls behind). */
#define RING_SIZE		0x40000
#define RING_MASK		(RING_SIZE - 1)
#define FILL_CHUNK		0x4000
#define WAIT_GIVEUP_MS	3000
#define SLOW_READ_MS	100		/* thread reads at least this long are logged */
#define SLOW_WAIT_MS	20		/* core waits at least this long are logged */

static unsigned char ring[RING_SIZE];
static unsigned char fillBuf[FILL_CHUNK];
static CRITICAL_SECTION ringLock;
static HANDLE readThread, workEvent, readyEvent;
static volatile int readQuit;
/* under ringLock */
static int streamGen;			/* bumped by each restart */
static int streamPos;			/* ring holds [streamPos, streamEnd) of the .bin */
static int streamEnd;
static int streamEof;
static int cdPos;				/* where the core's next read starts (core thread) */
static unsigned char iplSector[PCE_CD_IPL_SIZE];	/* see pceCdIpl() */
static int iplValid;

unsigned pceCdReadMs, pceCdReadMaxMs, pceCdReads, pceCdSeeks;
unsigned pceCdWaitMs, pceCdWaitMaxMs, pceCdWaits;

/* Points the stream at pos (under ringLock). */
static void streamRestart(int pos)
{
	streamGen++;
	streamPos = streamEnd = pos;
	streamEof = 0;
}

static DWORD WINAPI readThreadProc(LPVOID arg)
{
	int gen = -1, filePos = -1, pos;	/* -1: seek before the first read */
	DWORD got, t0, ms;

	(void)arg;
	while (!readQuit) {
		EnterCriticalSection(&ringLock);
		if (streamEof || streamEnd - streamPos > RING_SIZE - FILL_CHUNK) {
			LeaveCriticalSection(&ringLock);
			WaitForSingleObject(workEvent, INFINITE);
			continue;
		}
		gen = streamGen;
		pos = streamEnd;
		LeaveCriticalSection(&ringLock);

		t0 = GetTickCount();
		if (filePos != pos) {
			SetFilePointer(cdHandle, pos, NULL, FILE_BEGIN);
			pceCdSeeks++;
		}
		got = 0;
		ReadFile(cdHandle, fillBuf, FILL_CHUNK, &got, NULL);
		filePos = pos + (int)got;
		ms = GetTickCount() - t0;
		pceCdReadMs += ms;
		if (ms > pceCdReadMaxMs)
			pceCdReadMaxMs = ms;
		pceCdReads++;
		if (ms >= SLOW_READ_MS)
			CeLog("pce cd: slow read %lums at 0x%08X", (unsigned long)ms, pos);

		EnterCriticalSection(&ringLock);
		if (gen == streamGen && pos == streamEnd) {
			int at = pos & RING_MASK, n = (int)got;

			if (at + n > RING_SIZE) {
				memcpy(ring + at, fillBuf, RING_SIZE - at);
				memcpy(ring, fillBuf + RING_SIZE - at, n - (RING_SIZE - at));
			} else {
				memcpy(ring + at, fillBuf, n);
			}
			streamEnd += n;
			if (got < FILL_CHUNK)
				streamEof = 1;
		}
		LeaveCriticalSection(&ringLock);
		SetEvent(readyEvent);
	}
	return 0;
}

static void readerStart(void)
{
	InitializeCriticalSection(&ringLock);
	workEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	readyEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
	readQuit = 0;
	streamRestart(0);
	cdPos = 0;
	readThread = CreateThread(NULL, 0, readThreadProc, NULL, 0, NULL);
	if (!readThread)
		CeLog("pce cd: cannot start the read-ahead thread");
	else
		/* Above the frame loop (normal), below the audio thread (highest).
		 * At the same priority as the frame loop, which does not sleep
		 * when a frame runs long, the reads (the file system work is done
		 * on the CPU) got the CPU only between its 100ms quanta: a 16KB
		 * read took 100-1200ms and the core waited up to 0.9s. The thread
		 * mostly waits on the storage, so it takes little from the loop. */
		SetThreadPriority(readThread, THREAD_PRIORITY_ABOVE_NORMAL);
}

void *pceCdReaderThread(void)
{
	return readThread;
}

static void readerStop(void)
{
	if (!readThread)
		return;
	readQuit = 1;
	SetEvent(workEvent);
	WaitForSingleObject(readThread, INFINITE);
	CloseHandle(readThread);
	CloseHandle(workEvent);
	CloseHandle(readyEvent);
	DeleteCriticalSection(&ringLock);
	readThread = NULL;
}

/* Copies len bytes from cdPos on out of the ring, restarting the stream
 * if cdPos is not in it and waiting for the thread if the bytes are not
 * there yet. Past the end of the file it gives zeros. */
static void cdRead(void *dest, int len, const char *kind)
{
	unsigned char *d = dest;
	DWORD t0 = 0, ms;
	int waited = 0, n, at;

	if (!readThread) {
		memset(d, 0, len);
		cdPos += len;
		return;
	}
	while (len > 0) {
		EnterCriticalSection(&ringLock);
		if (cdPos < streamPos || cdPos > streamEnd) {
			streamRestart(cdPos);
			SetEvent(workEvent);
		}
		n = streamEnd - cdPos;
		if (n > 0) {
			if (n > len)
				n = len;
			at = cdPos & RING_MASK;
			if (at + n > RING_SIZE) {
				memcpy(d, ring + at, RING_SIZE - at);
				memcpy(d + RING_SIZE - at, ring, n - (RING_SIZE - at));
			} else {
				memcpy(d, ring + at, n);
			}
			d += n;
			len -= n;
			cdPos += n;
			streamPos = cdPos;		/* frees the space behind */
			LeaveCriticalSection(&ringLock);
			SetEvent(workEvent);
			continue;
		}
		if (streamEof && cdPos >= streamEnd) {
			LeaveCriticalSection(&ringLock);
			memset(d, 0, len);
			cdPos += len;
			break;
		}
		LeaveCriticalSection(&ringLock);
		if (!waited) {
			waited = 1;
			t0 = GetTickCount();
		} else if (GetTickCount() - t0 > WAIT_GIVEUP_MS) {
			memset(d, 0, len);
			cdPos += len;
			break;
		}
		WaitForSingleObject(readyEvent, 50);
	}
	if (waited) {
		ms = GetTickCount() - t0;
		pceCdWaitMs += ms;
		if (ms > pceCdWaitMaxMs)
			pceCdWaitMaxMs = ms;
		pceCdWaits++;
		if (ms >= SLOW_WAIT_MS)
			CeLog("pce cd: core waited %lums for %s at 0x%08X", (unsigned long)ms, kind, cdPos);
	}
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
	size_t len = strlen(src);

	if (size) {
		size_t n = len < size - 1 ? len : size - 1;
		memcpy(dst, src, n);
		dst[n] = 0;
	}
	return len;
}

int CD_ReadByte(void)
{
	int i;

	if (cdDataLeft == 0) {
		if (cdDatatrackMode == 8)
			cdRead(cdBuffer, 2352, "data");
		else
			cdRead(&cdBuffer[16], 2048, "data");
		cdDataLeft = 2048;
	}
	i = (unsigned char)cdBuffer[2064 - cdDataLeft];
	cdDataLeft--;
	return i;
}

/* Tops up the CD audio ring (Sound.s drains it from cdReadPtr), at most
 * 0xC00 bytes per call; past the end of the file it fills with silence. */
void CD_FillBuffer(void)
{
	int ptr, len, left = 0xC00;

	while ((len = cdReadPtr + (int)sizeof(cdBuffer) - cdWritePtr) > 0) {
		if (len > left)
			len = left;
		ptr = cdWritePtr & (sizeof(cdBuffer) - 1);
		if (len + ptr > (int)sizeof(cdBuffer))
			len = sizeof(cdBuffer) - ptr;
		cdRead(&cdBuffer[ptr], len, "audio");
		left -= len;
		cdWritePtr += len;
		if (left <= 0)
			return;
	}
}

void CD_SeekPos(int pos)
{
	cdPos = pos;
	cdDataLeft = 0;
	if (readThread) {
		EnterCriticalSection(&ringLock);
		if (pos < streamPos || pos > streamEnd) {
			streamRestart(pos);
			SetEvent(workEvent);
		}
		LeaveCriticalSection(&ringLock);
	}
}

void CD_ResetBuffer(void)
{
	cdReadPtr = 0;
	cdWritePtr = 0;
}

/* --- CD-ROM command log ---------------------------------------------------
 * cdrom.s reports each SCSI command and some ADPCM register writes through
 * Shared/AsmExtra.s:debugOutput_asm, which calls this while gDebugSet is
 * set (pce_core_ce.c sets it for CD games while debug logging is on). The
 * last command's bytes (scsiCommandHex, made global by CE/core/cefix.sed)
 * go with each line. A message repeated back to back is counted, not
 * written again. */
extern char scsiCommandHex[];

void debugOutput(const char *str)
{
	static char last[64], lastCmd[40];
	static unsigned repeats;

	if (strcmp(str, last) == 0 && strcmp(scsiCommandHex, lastCmd) == 0) {
		repeats++;
		return;
	}
	if (repeats)
		CeLog("pce cd: (last line repeated %u more times)", repeats);
	repeats = 0;
	strlcpy(last, str, sizeof(last));
	strlcpy(lastCmd, scsiCommandHex, sizeof(lastCmd));
	CeLog("pce cd: t=%lu %s [cmd %s]", (unsigned long)GetTickCount(), str, scsiCommandHex);
}

/* --- CPU and drive state snapshot ------------------------------------------
 * For finding where a CD game waits when it stops sending commands. The
 * H6280 PC is sampled at the end of each frame (cpu.s has stored the
 * registers into h6280OpTable's state block by then); every
 * STATE_WINDOW frames the most frequent PCs are logged with the code
 * around the top one, the stack, the IRQ lines and the drive/ADPCM
 * registers. Offsets are those of ARMH6280/H6280.i (state block below
 * h6280OpTable) and of cdrom.s's cdromState (checked with nm). */
extern unsigned char h6280OpTable[];
extern unsigned char pceRAM[];

#define STATE_WINDOW	120
#define PC_SLOTS		8

/* the state block is below the label (H6280.i: .struct -128) */
#define CPU_BLOCK		((const unsigned char *)((uintptr_t)h6280OpTable - 128))
#define CPU_U32(off)	(*(const u32 *)(CPU_BLOCK + 128 + (off)))
#define CPU_U8(off)		(CPU_BLOCK[128 + (off)])
#define H_SP			(-112)
#define H_CYCLES		(-108)
#define H_PC			(-104)
#define H_IRQPENDING	(-96)
#define H_MPR			(-92)
#define H_IRQDISABLE	(-78)
#define H_LASTBANK		(-72)
#define H_ROMMAP		(-48)

#define CD_U32(off)		(*(const u32 *)(cdromState + (off)))
#define CD_U8(off)		(cdromState[(off)])

static unsigned readCpuByte(unsigned addr)
{
	const unsigned char *base = *(unsigned char * const *)(CPU_BLOCK + 128 + H_ROMMAP + ((addr >> 13) & 7) * 4);

	/* $0000-$1FFF is I/O: reading it through the map is not memory */
	if (!(addr & 0xE000) || !base)
		return 0;
	return base[addr & 0xFFFF];
}

void pceCdDebugFrame(void)
{
	static unsigned short pcs[PC_SLOTS];
	static unsigned counts[PC_SLOTS], frames, other;
	unsigned pc, i, top, sp;
	char code[3 * 24 + 1], stack[3 * 8 + 1];

	/* h6280RegPC is h6280pc plus the bank's base (h6280LastBank) */
	pc = (CPU_U32(H_PC) - CPU_U32(H_LASTBANK)) & 0xFFFF;
	for (i = 0; i < PC_SLOTS; i++) {
		if (counts[i] && pcs[i] == pc)
			break;
		if (!counts[i]) {
			pcs[i] = (unsigned short)pc;
			break;
		}
	}
	if (i < PC_SLOTS)
		counts[i]++;
	else
		other++;
	if (++frames < STATE_WINDOW)
		return;

	top = 0;
	for (i = 1; i < PC_SLOTS; i++)
		if (counts[i] > counts[top])
			top = i;
	for (i = 0; i < 24; i++)
		sprintf(code + i * 3, "%02X ", readCpuByte((pcs[top] - 8 + i) & 0xFFFF));
	sp = CPU_U32(H_SP) >> 24;
	for (i = 0; i < 8; i++)
		sprintf(stack + i * 3, "%02X ", pceRAM[0x100 + ((sp + 1 + i) & 0xFF)]);
	CeLog("pce cd state: pc %04X x%u, %04X x%u, %04X x%u, %04X x%u, other %u; mpr %02X %02X %02X %02X %02X %02X %02X %02X",
		pcs[0], counts[0], pcs[1], counts[1], pcs[2], counts[2], pcs[3], counts[3], other,
		CPU_U8(H_MPR), CPU_U8(H_MPR + 1), CPU_U8(H_MPR + 2), CPU_U8(H_MPR + 3),
		CPU_U8(H_MPR + 4), CPU_U8(H_MPR + 5), CPU_U8(H_MPR + 6), CPU_U8(H_MPR + 7));
	CeLog("pce cd state: code@%04X-8: %s; sp %02X: %s; I=%u irqPend %02X irqDis %02X",
		pcs[top], code, sp, stack, (CPU_U32(H_CYCLES) & 0x04) ? 1 : 0,
		CPU_U8(H_IRQPENDING), CPU_U8(H_IRQDISABLE));
	CeLog("pce cd state: scsi %02X cmd %02X dataLen %X seek %u; irqReq %02X irqMask %02X;"
		" adCtrl %02X adDma %02X adStat %02X adDmaOn %02X adRate %02X adPtr %08X adLen %08X adWr %08X adRd %08X;"
		" fade %02X playMode %u cdAudio %u",
		CD_U8(0x4C), CD_U8(0x5F), CD_U32(0x0C), CD_U32(0x1C), CD_U8(0x50), CD_U8(0x4F),
		CD_U8(0x55), CD_U8(0x54), CD_U8(0x58), CD_U8(0x59), CD_U8(0x56),
		CD_U32(0x20), CD_U32(0x24), CD_U32(0x28), CD_U32(0x2C),
		CD_U8(0x5A), CD_U8(0x5B), CD_U8(0x5C));

	memset(counts, 0, sizeof(counts));
	frames = other = 0;
}

/* --- opening a .cue/.bin pair ---------------------------------------------- */
void pceCdClose(void)
{
	readerStop();
	if (cdHandle != INVALID_HANDLE_VALUE)
		CloseHandle(cdHandle);
	cdHandle = INVALID_HANDLE_VALUE;
	cdInserted = 0;
	cdIsBinCue = 0;
}

/* Reads the whole (small) cue sheet as a NUL-terminated string. */
static int readCueText(const WCHAR *path, char *buf, DWORD size)
{
	HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	DWORD got = 0;

	if (h == INVALID_HANDLE_VALUE)
		return 0;
	ReadFile(h, buf, size - 1, &got, NULL);
	CloseHandle(h);
	buf[got] = 0;
	return got != 0;
}

/* A FILE name from the cue sheet in the encoding it was written in:
 * UTF-8 if it is valid UTF-8, else the device code page (Shift-JIS for
 * sheets written on Japanese Windows). */
static void cueNameToWide(const char *name, WCHAR *out, int count)
{
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, out, count)
		&& !MultiByteToWideChar(CP_ACP, 0, name, -1, out, count))
		out[0] = 0;
	out[count - 1] = 0;
}

/* <folder of cuePath>\<name> (name may use / or \ for a subfolder) */
static void binPathFromCue(const WCHAR *cuePath, const char *name, WCHAR *out, int count)
{
	WCHAR nameW[MAX_PATH];
	const WCHAR *slash = wcsrchr(cuePath, L'\\');
	int dirLen = slash ? (int)(slash - cuePath) + 1 : 0;
	WCHAR *p;

	cueNameToWide(name, nameW, MAX_PATH);
	for (p = nameW; *p; p++)
		if (*p == L'/')
			*p = L'\\';
	if (nameW[0] == L'\\' || dirLen >= count) {
		wcsncpy(out, nameW, count - 1);		/* absolute already */
	} else {
		wcsncpy(out, cuePath, dirLen);
		wcsncpy(out + dirLen, nameW, count - 1 - dirLen);
	}
	out[count - 1] = 0;
}

/* The IPL: the data track's second sector, which the System Card boots
 * from. It holds the game's name at 0x6A (e.g. "GAROU SPECIAL   by Ken";
 * some games leave "SAMPLE PROGRAM" there) and where to load the program.
 * Read once, before the reader thread owns the handle. */
static void readIpl(int dataStart, int sectorSize)
{
	char title[23];
	DWORD got = 0;
	int i;

	iplValid = 0;
	if (dataStart < 0)
		return;
	/* user data follows the 16 byte sync/header of a raw sector */
	if (SetFilePointer(cdHandle, dataStart + sectorSize + (sectorSize == 2352 ? 16 : 0),
			NULL, FILE_BEGIN) == 0xFFFFFFFF
		|| !ReadFile(cdHandle, iplSector, sizeof(iplSector), &got, NULL)
		|| got != sizeof(iplSector)
		|| memcmp(iplSector + 0x20, "PC Engine CD-ROM SYSTEM", 23) != 0) {
		CeLog("pce cd: no IPL in the data track");
		return;
	}
	iplValid = 1;
	for (i = 0; i < 22; i++) {
		unsigned char c = iplSector[0x6A + i];
		title[i] = (char)(c >= 0x20 && c < 0x7F ? c : '?');
	}
	title[22] = 0;
	CeLog("pce cd: IPL title \"%s\"", title);
}

const unsigned char *pceCdIpl(void)
{
	return iplValid ? iplSector : NULL;
}

int pceCdOpenCue(const WCHAR *cuePath)
{
	static char cue[CUE_MAX_SIZE];
	char binName[FILENAMELEN] = "";
	WCHAR binPath[MAX_PATH];
	const CUETrackInfo *t;
	int count = 0;
	int dataStart = -1;		/* byte offset of the first data sector in the .bin */
	int dataSector = 2048;
	DWORD size;

	pceCdClose();
	if (!readCueText(cuePath, cue, sizeof(cue))) {
		CeLog("pce cd: cannot read the cue sheet");
		return 0;
	}

	/* FileHandling.c:CD_ConvertCueFile. The .bin is the file of the first
	 * data track; track offsets are in bytes from the start of it. */
	memset(&cdRomToc, 0, sizeof(cdRomToc));
	memcpy(cdRomToc.magic, "TGCD0100", 8);
	cdDatatrackMode = 4;
	startParser(cue);
	while ((t = nextTrack(0)) != NULL && count < 99) {
		CD_TRACK *track = &cdRomToc.tracks[count++];
		uint32_t lba = t->dataStart;

		if (t->fileIndex > 1) {
			CeLog("pce cd: cue sheets with more than one FILE are not supported");
			return 0;
		}
		if (!binName[0] && t->trackMode != TRK_MODE_AUDIO) {
			strlcpy(binName, t->filename, sizeof(binName));
			dataStart = (int)t->fileOffset;
			dataSector = t->trackMode == TRK_MODE_MODE1_2352 ? 2352 : 2048;
		}
		if (t->trackMode == TRK_MODE_AUDIO)
			track->mode = 0;
		else if (t->trackMode == TRK_MODE_MODE1_2352)
			cdDatatrackMode = track->mode = 8;
		else
			cdDatatrackMode = track->mode = 4;
		track->LBA0 = (u8)(lba >> 16);
		track->LBA1 = (u8)(lba >> 8);
		track->LBA2 = (u8)lba;
		track->start = t->fileOffset;
	}
	cdRomToc.trackCount = (u8)count;
	if (count == 0 || !binName[0]) {
		CeLog("pce cd: no data track in the cue sheet");
		return 0;
	}

	binPathFromCue(cuePath, binName, binPath, MAX_PATH);
	cdHandle = CreateFileW(binPath, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (cdHandle == INVALID_HANDLE_VALUE) {
		CeLog("pce cd: cannot open the .bin named in the cue sheet (%s)", binName);
		return 0;
	}
	size = GetFileSize(cdHandle, NULL);
	if (size == 0xFFFFFFFF || size > 0x7FFFFFFF) {
		CeLog("pce cd: .bin size not usable");
		pceCdClose();
		return 0;
	}
	cdFileSize = (int)size;
	readIpl(dataStart, dataSector);
	cdIsBinCue = 1;
	cdInserted = 1;
	cdReadPtr = cdWritePtr = cdDataLeft = 0;
	readerStart();
	CeLog("pce cd: %d tracks, %u byte .bin, data sectors %d bytes",
		count, (unsigned)size, cdDatatrackMode == 8 ? 2352 : 2048);
	return 1;
}

/* --- save state ------------------------------------------------------------ */
/* The drive state block, the CD RAMs, and where the reader is in the
 * .bin (upstream's pcePackState has no CD part). The Arcade Card is a
 * separate block (pceAcSaveState below). */
#define CD_STATE_TAG	0x44434350		/* "PCCD" */

size_t pceCdStateSize(void)
{
	return 4 + (size_t)(cdromStateEnd - cdromState) + CD_RAM_TOTAL
		+ 4 * 4 + sizeof(cdBuffer);
}

static unsigned char *put32(unsigned char *p, int v)
{
	memcpy(p, &v, 4);
	return p + 4;
}

static const unsigned char *get32(const unsigned char *p, int *v)
{
	memcpy(v, p, 4);
	return p + 4;
}

void pceCdSaveState(void *dest)
{
	unsigned char *p = dest;
	size_t n = cdromStateEnd - cdromState;
	int pos = cdPos;

	p = put32(p, CD_STATE_TAG);
	memcpy(p, cdromState, n);
	p += n;
	memcpy(p, CD_PCM_RAM, CD_RAM_TOTAL);
	p += CD_RAM_TOTAL;
	p = put32(p, cdReadPtr);
	p = put32(p, cdWritePtr);
	p = put32(p, cdDataLeft);
	p = put32(p, pos);
	memcpy(p, cdBuffer, sizeof(cdBuffer));
}

int pceCdStateValid(const void *src)
{
	int tag;

	get32(src, &tag);
	return tag == CD_STATE_TAG;
}

int pceCdLoadState(const void *src)
{
	const unsigned char *p = src;
	size_t n = cdromStateEnd - cdromState;
	int tag, pos;

	p = get32(p, &tag);
	if (tag != CD_STATE_TAG)
		return 0;
	memcpy(cdromState, p, n);
	p += n;
	memcpy(CD_PCM_RAM, p, CD_RAM_TOTAL);
	p += CD_RAM_TOTAL;
	p = get32(p, &cdReadPtr);
	p = get32(p, &cdWritePtr);
	p = get32(p, &cdDataLeft);
	p = get32(p, &pos);
	memcpy(cdBuffer, p, sizeof(cdBuffer));
	tag = cdDataLeft;
	CD_SeekPos(pos);	/* the stream follows (and clears cdDataLeft) */
	cdDataLeft = tag;
	return 1;
}

/* --- Arcade Card save state ------------------------------------------------
 * The registers (ArcadeCard.s: acPort0-3, 16 bytes each, then the shift
 * register, the shift and rotate amounts and 2 pad bytes) and the 2MB of
 * RAM. The RAM goes in 16KB blocks, and blocks that are all zero are left
 * out (a mask says which are there): most games use a small part of it,
 * and the whole 2MB would take seconds to write to the device's flash. */
#define AC_STATE_TAG	0x43414350		/* "PCAC" */
#define AC_REGS_SIZE	72
#define AC_RAM_SIZE		0x200000
#define AC_BLOCK		0x4000
#define AC_BLOCKS		(AC_RAM_SIZE / AC_BLOCK)
#define AC_MASK_WORDS	(AC_BLOCKS / 32)

extern unsigned char acPort0[];
extern unsigned char ACC_RAM[];

size_t pceAcStateMaxSize(void)
{
	return 4 + AC_REGS_SIZE + AC_MASK_WORDS * 4 + AC_RAM_SIZE;
}

static int acBlockUsed(int b)
{
	const unsigned *w = (const unsigned *)(ACC_RAM + b * AC_BLOCK);
	int i;

	for (i = 0; i < AC_BLOCK / 4; i++)
		if (w[i])
			return 1;
	return 0;
}

size_t pceAcSaveState(void *dest)
{
	unsigned char *p = dest, *maskAt;
	unsigned mask[AC_MASK_WORDS];
	int b;

	p = put32(p, AC_STATE_TAG);
	memcpy(p, acPort0, AC_REGS_SIZE);
	p += AC_REGS_SIZE;
	maskAt = p;
	p += sizeof(mask);
	memset(mask, 0, sizeof(mask));
	for (b = 0; b < AC_BLOCKS; b++) {
		if (!acBlockUsed(b))
			continue;
		mask[b / 32] |= 1u << (b % 32);
		memcpy(p, ACC_RAM + b * AC_BLOCK, AC_BLOCK);
		p += AC_BLOCK;
	}
	memcpy(maskAt, mask, sizeof(mask));
	return (size_t)(p - (unsigned char *)dest);
}

int pceAcStateValid(const void *src, size_t size)
{
	const unsigned char *p = src;
	unsigned mask[AC_MASK_WORDS];
	size_t need = 4 + AC_REGS_SIZE + sizeof(mask);
	int tag, b;

	if (size < need)
		return 0;
	p = get32(p, &tag);
	if (tag != AC_STATE_TAG)
		return 0;
	memcpy(mask, p + AC_REGS_SIZE, sizeof(mask));
	for (b = 0; b < AC_BLOCKS; b++)
		if (mask[b / 32] & (1u << (b % 32)))
			need += AC_BLOCK;
	return size >= need;
}

int pceAcLoadState(const void *src, size_t size)
{
	const unsigned char *p = src;
	unsigned mask[AC_MASK_WORDS];
	int b;

	if (!pceAcStateValid(src, size))
		return 0;
	p += 4;
	memcpy(mask, p + AC_REGS_SIZE, sizeof(mask));
	memcpy(acPort0, p, AC_REGS_SIZE);
	p += AC_REGS_SIZE + sizeof(mask);
	for (b = 0; b < AC_BLOCKS; b++) {
		if (mask[b / 32] & (1u << (b % 32))) {
			memcpy(ACC_RAM + b * AC_BLOCK, p, AC_BLOCK);
			p += AC_BLOCK;
		} else {
			memset(ACC_RAM + b * AC_BLOCK, 0, AC_BLOCK);
		}
	}
	return 1;
}

