// KVM (Windows): quality that depends on motion, sharpening of still tiles, congestion regulator (WA-2). See kvm_adapt.h.
//
// Everything here runs on the main loop thread of the KVM process (the tile loop, the pipe writes and the pause are all there), with one exception:
// the input thread changes encCompression / encType (read here as plain ints) and answers a refresh by clearing tileInfo[][].crc / flags (not touched here).

#include "tile.h"

#ifdef KVM_ADAPT_ENABLED

#include <stdio.h>
#include <string.h>
#include <timeapi.h>

extern "C"
{
extern int TILE_WIDTH;
extern int TILE_HEIGHT;
extern int TILE_WIDTH_COUNT;
extern int TILE_HEIGHT_COUNT;
extern int PIXEL_SIZE;
extern int FRAME_RATE_TIMER;
extern tileInfo_t **tileInfo;
int kvm_adapt_encode_sharp(int x, int y, int w, int h, void *desktop, void **buffer, long long *bufferSize, int *quality); // tile.cpp
}
extern ULONG encCompression; // tile.cpp
extern int encType;          // tile.cpp

static struct
{
	int sessionOn, envOff;
	int active;						// WA-2 applies to the frame being processed
	int qUser;
	unsigned char stamp;
	DWORD now, lastFrameTick, sessionStart, lastContentChange;
	int changedThisFrame, prevChanged;
	int level, levelFrame, calm, hot;
	DWORD winStart;
	long long winWriteUs, winBytes;
	double acceptRate;				// bytes per second the link took while it was saturated (0 = unknown)
	double tokens; DWORD tokenTick;	// rate limiter (level 4 and up)
	int lossyCount; int dueIn;		// lossy tiles; ms until the first of them is due for sharpening
	int cursor;						// next tile to look at for sharpening
	// statistics of the period
	int levelMax, wantImage, rateSkips, pauseMult;
	long long rectsMotion, rectsFull, bytesMotion, bytesFull, sharpenRects, sharpenTiles, sharpenBytes, settleSum, settleN, settleMax;
	int blockPctMax;
} gA;

static int clamp_q(unsigned long q) { return (q < 1) ? 1 : (q > 90 ? 90 : (int)q); }

static int motion_quality(int level)
{
	int q = KVM_ADAPT_Q_MOTION - level * KVM_ADAPT_Q_STEP;
	return q < KVM_ADAPT_Q_MOTION_MIN ? KVM_ADAPT_Q_MOTION_MIN : q;
}

static int type_ok(void) { return (encType == 1 || encType == 4) && (PIXEL_SIZE == 3 || PIXEL_SIZE == 4); }

static int is_lossy(const tileInfo_t *t, int qUser)
{
	return t->sentQ != 0 && (t->sentQ < qUser || t->sentSub);
}

static int is_candidate(const tileInfo_t *t)
{
	return is_lossy(t, gA.qUser) && (DWORD)(gA.now - t->lastChange) >= KVM_ADAPT_SETTLE_MS && t->flags != (char)TILE_MARKED_NOT_SENT && t->crc == t->contentCrc;
}

static double rate_limit(void) { return gA.level >= 4 ? gA.acceptRate * KVM_ADAPT_RATE_FRACTION_PCT / 100.0 : 0.0; }

static void refill_tokens(DWORD now)
{
	double rate = rate_limit();
	if (rate <= 0.0) { gA.tokens = 0.0; gA.tokenTick = now; return; }
	gA.tokens += rate * (double)(DWORD)(now - gA.tokenTick) / 1000.0;
	if (gA.tokens > rate) { gA.tokens = rate; }					// at most one second of burst
	if (gA.tokens < -2.0 * rate) { gA.tokens = -2.0 * rate; }
	gA.tokenTick = now;
}

extern "C"
{

void kvm_adapt_session_start(void)
{
	char env[8] = "";
	memset(&gA, 0, sizeof(gA));
	gA.envOff = (GetEnvironmentVariableA("MESH_KVM_ADAPT", env, sizeof(env)) > 0 && env[0] == '0') ? 1 : 0;
	gA.sessionOn = 1;
	gA.sessionStart = gA.winStart = gA.lastFrameTick = gA.tokenTick = timeGetTime();
	gA.now = gA.sessionStart;
	gA.qUser = clamp_q(encCompression);
	gA.dueIn = 0x7FFFFFFF;
	kvm_stats_log("adapt start enabled=%d reason=%s q_user=%d type=%d q_motion=%d motion_ms=%d settle_ms=%d", gA.envOff ? 0 : 1, gA.envOff ? "env" : "on", gA.qUser, encType, KVM_ADAPT_Q_MOTION, KVM_ADAPT_MOTION_MS, KVM_ADAPT_SETTLE_MS);
}

void kvm_adapt_session_end(void)
{
	gA.sessionOn = 0;
}

// Regulator window and review of the tiles; called at the start of every iteration of the loop.
void kvm_adapt_iteration(void)
{
	DWORD now;
	if (!gA.sessionOn) { return; }
	now = timeGetTime();
	gA.now = now;

	if ((DWORD)(now - gA.winStart) >= KVM_ADAPT_WINDOW_MS)
	{
		double windowUs = (double)(DWORD)(now - gA.winStart) * 1000.0, frac = (double)gA.winWriteUs / windowUs;
		int pct = (int)(frac * 100.0), from = gA.level;
		if (pct > gA.blockPctMax) { gA.blockPctMax = pct; }
		if (frac >= 0.5 && gA.winBytes > 0)
		{
			double rate = (double)gA.winBytes * 1000.0 / (double)(DWORD)(now - gA.winStart); // the link took this much while it was the bottleneck
			gA.acceptRate = (gA.acceptRate > 0.0) ? (0.5 * gA.acceptRate + 0.5 * rate) : rate;
		}
		if (frac * 100.0 >= KVM_ADAPT_CONGEST_PCT)
		{
			gA.calm = 0;
			// From level 0 only after two overloaded windows in a row: the first full image of a session (or a REFRESH) blocks the pipe write
			// for one window although the link is fine; real congestion lasts longer.
			if (gA.level > 0 || ++gA.hot >= 2) { if (gA.level < KVM_ADAPT_MAX_LEVEL) { gA.level++; } }
		}
		else if (frac * 100.0 <= KVM_ADAPT_CALM_PCT)
		{
			gA.hot = 0;
			if (++gA.calm >= KVM_ADAPT_CALM_WINDOWS && gA.level > 0) { gA.level--; gA.calm = 0; }
		}
		else
		{
			gA.calm = 0;
			gA.hot = 0;
		}
		if (gA.level == 0) { gA.acceptRate = 0.0; }
		if (gA.level > gA.levelMax) { gA.levelMax = gA.level; }
		if (gA.level != from)
		{
			kvm_stats_log("adapt level from=%d to=%d block=%d.%d%% accept_kbps=%d q_motion=%d since_start_ms=%lu", from, gA.level, pct, (int)(frac * 1000.0) % 10, (int)(gA.acceptRate * 8.0 / 1000.0), motion_quality(gA.level), (unsigned long)(now - gA.sessionStart));
			if (gA.level >= 4 && from < 4) { gA.tokens = 0.0; gA.tokenTick = now; kvm_stats_log("adapt rate_cap kbps=%d", (int)(rate_limit() * 8.0 / 1000.0)); }
		}
		gA.winStart = now;
		gA.winWriteUs = 0;
		gA.winBytes = 0;
	}

	// Review: how many tiles are lossy and when the first one is due for sharpening
	gA.lossyCount = 0;
	gA.dueIn = 0x7FFFFFFF;
	if (tileInfo != NULL && TILE_WIDTH_COUNT > 0)
	{
		int qUser = clamp_q(encCompression);
		for (int row = 0; row < TILE_HEIGHT_COUNT; row++)
		{
			const tileInfo_t *r = tileInfo[row];
			for (int col = 0; col < TILE_WIDTH_COUNT; col++)
			{
				if (is_lossy(&r[col], qUser) && r[col].crc == r[col].contentCrc)
				{
					int due = (int)(r[col].lastChange + KVM_ADAPT_SETTLE_MS - now);
					gA.lossyCount++;
					if (due < gA.dueIn) { gA.dueIn = due; }
				}
			}
		}
	}
}

int kvm_adapt_want_image(void)
{
	if (!gA.sessionOn || gA.envOff || !type_ok()) { return 0; }
	if (gA.lossyCount > 0 && gA.dueIn <= 0 && (DWORD)(gA.now - gA.lastContentChange) >= KVM_ADAPT_SETTLE_MS) // while the screen changes, new frames come by themselves
	{
		gA.wantImage++;
		return 1;
	}
	return 0;
}

void kvm_adapt_frame_begin(void)
{
	DWORD now = timeGetTime();
	gA.now = now;
	gA.qUser = clamp_q(encCompression);
	gA.active = (gA.sessionOn && !gA.envOff && type_ok()) ? 1 : 0;
	if (++gA.stamp == 0) { gA.stamp = 1; } // 0 is the value of a tile that was never looked at
	gA.prevChanged = ((DWORD)(now - gA.lastFrameTick) > KVM_ADAPT_MOTION_MS) ? 0 : gA.changedThisFrame;
	gA.changedThisFrame = 0;
	gA.lastFrameTick = now;
	gA.levelFrame = gA.level;
}

void kvm_adapt_note_crc(struct tileInfo_t *t, int crc)
{
	if (!gA.active || t->noted == gA.stamp) { return; }
	t->noted = gA.stamp;
	if (t->crc == 0xFF) { t->refresh = 1; } // sent only because the viewer was reset, not because of a change
	if (crc != t->contentCrc)
	{
		t->streak = (t->streak > 0 && (DWORD)(gA.now - t->lastChange) <= KVM_ADAPT_MOTION_MS) ? (unsigned char)(t->streak < 255 ? t->streak + 1 : 255) : 1;
		t->lastChange = gA.now;
		t->contentCrc = crc;
		gA.changedThisFrame++;
		gA.lastContentChange = gA.now;
	}
}

int kvm_adapt_class(const struct tileInfo_t *t)
{
	if (!gA.active) { return 0; }
	if (t->streak >= KVM_ADAPT_MOTION_STREAK && (DWORD)(gA.now - t->lastChange) <= KVM_ADAPT_MOTION_MS && gA.prevChanged >= KVM_ADAPT_MOTION_TILES) { return 1; }
	if (gA.levelFrame >= 1 && t->refresh) { return 1; } // under congestion a refresh is sent in the motion class and sharpened later
	return 0;
}

void kvm_adapt_params(int cls, int motionSub, int fullSub, int *q, int *sub)
{
	if (cls == 1 && gA.active)
	{
		int qm = motion_quality(gA.levelFrame);
		*q = gA.qUser < qm ? gA.qUser : qm;
		*sub = motionSub;
	}
	else
	{
		*q = gA.active ? gA.qUser : clamp_q(encCompression);
		*sub = fullSub;
	}
}

void kvm_adapt_sent(int row0, int col0, int row1, int col1, int cls, int q, int lossySub, long long bytes)
{
	for (int r = row0; r <= row1; r++)
	{
		for (int c = col0; c <= col1; c++)
		{
			tileInfo_t *t = &tileInfo[r][c];
			if (gA.active) { t->sentQ = (unsigned char)q; t->sentSub = (unsigned char)(lossySub ? 1 : 0); } else { t->sentQ = 100; t->sentSub = 0; } // not JPEG: nothing to sharpen
			if (t->refresh && cls == 1) { t->lastChange = gA.now; } // a refresh sent lossy waits for the settle time, it is not sharpened in the same frame
			t->refresh = 0;
		}
	}
	if (cls == 1) { gA.rectsMotion++; gA.bytesMotion += bytes; } else { gA.rectsFull++; gA.bytesFull += bytes; }
}

long long kvm_adapt_sharpen_budget(void)
{
	long long b;
	if (!gA.active) { return 0; }
	b = KVM_ADAPT_SHARPEN_BYTES;
	if (gA.level >= 4) { b /= 4; } else if (gA.level >= 1) { b /= 2; }
	if (gA.level >= 4 && rate_limit() > 0.0)
	{
		refill_tokens(timeGetTime());
		if (gA.tokens <= 0.0) { return 0; }
		if ((double)b > gA.tokens) { b = (long long)gA.tokens; }
	}
	return b;
}

int kvm_adapt_sharpen_next(void *desktop, long long budgetLeft, void **packet, long long *size)
{
	int total, n;
	if (!gA.active || budgetLeft <= 0 || tileInfo == NULL || TILE_WIDTH_COUNT <= 0) { return 0; }
	total = TILE_WIDTH_COUNT * TILE_HEIGHT_COUNT;
	if (gA.cursor >= total || gA.cursor < 0) { gA.cursor = 0; }
	for (n = 0; n < total; n++)
	{
		int idx = (gA.cursor + n) % total, row = idx / TILE_WIDTH_COUNT, col = idx % TILE_WIDTH_COUNT, run = 1, q = 0;
		void *buf = NULL;
		long long sz = 0;
		if (!is_candidate(&tileInfo[row][col])) { continue; }
		while (run < KVM_ADAPT_SHARPEN_RUN && col + run < TILE_WIDTH_COUNT && is_candidate(&tileInfo[row][col + run])) { run++; }
		if (kvm_adapt_encode_sharp(col * TILE_WIDTH, row * TILE_HEIGHT, run * TILE_WIDTH, TILE_HEIGHT, desktop, &buf, &sz, &q) != 0 || buf == NULL)
		{
			// The encoder failed: wait another settle time before the next try
			for (int c = 0; c < run; c++) { tileInfo[row][col + c].lastChange = gA.now; }
			if (buf != NULL) { free(buf); }
			n += run - 1;
			continue;
		}
		for (int c = 0; c < run; c++)
		{
			tileInfo_t *t = &tileInfo[row][col + c];
			long long settle = (long long)(DWORD)(gA.now - t->lastChange);
			t->sentQ = (unsigned char)q; t->sentSub = 0; t->refresh = 0;
			gA.settleSum += settle; gA.settleN++;
			if (settle > gA.settleMax) { gA.settleMax = settle; }
		}
		gA.sharpenRects++; gA.sharpenTiles += run; gA.sharpenBytes += sz;
		gA.cursor = (idx + run) % total;
		*packet = buf;
		*size = sz;
		return 1;
	}
	return 0;
}

void kvm_adapt_note_write(unsigned int us, long long bytes)
{
	gA.winWriteUs += us;
	gA.winBytes += bytes;
	if (gA.level >= 4) { gA.tokens -= (double)bytes; }
}

int kvm_adapt_frame_pause(int base)
{
	int p = base, mult = (gA.level >= 5) ? 4 : 2;
	if (!gA.sessionOn || gA.envOff || gA.level < 4) { gA.pauseMult = 1; return base; } // up to level 3 only the quality of the motion class changes
	p = base * mult;
	if (p > KVM_ADAPT_MAX_PAUSE_MS) { p = (base > KVM_ADAPT_MAX_PAUSE_MS) ? base : KVM_ADAPT_MAX_PAUSE_MS; }
	if (rate_limit() > 0.0)
	{
		refill_tokens(timeGetTime());
		if (gA.tokens < 0.0)
		{
			int wait = (int)(-gA.tokens * 1000.0 / rate_limit());
			if (wait > KVM_ADAPT_MAX_PAUSE_MS) { wait = KVM_ADAPT_MAX_PAUSE_MS; }
			if (wait > p) { p = wait; gA.rateSkips++; }
		}
	}
	gA.pauseMult = mult;
	return p;
}

void kvm_adapt_stats_flush(int final)
{
	if (!gA.sessionOn) { return; }
	kvm_stats_log("adapt stats%s enabled=%d level_end=%d level_max=%d q_motion=%d rects_motion=%lld rects_full=%lld bytes_motion=%lld bytes_full=%lld sharpen_rects=%lld sharpen_tiles=%lld sharpen_bytes=%lld want_image=%d lossy_end=%d settle_ms_avg=%lld settle_ms_max=%lld block_pct_max=%d accept_kbps=%d pause_mult=%d rate_skips=%d",
		final ? " final=1" : "", gA.envOff ? 0 : 1, gA.level, gA.levelMax, motion_quality(gA.level), gA.rectsMotion, gA.rectsFull, gA.bytesMotion, gA.bytesFull, gA.sharpenRects, gA.sharpenTiles, gA.sharpenBytes,
		gA.wantImage, gA.lossyCount, gA.settleN > 0 ? gA.settleSum / gA.settleN : 0LL, gA.settleMax, gA.blockPctMax, (int)(gA.acceptRate * 8.0 / 1000.0), gA.pauseMult ? gA.pauseMult : 1, gA.rateSkips);
	gA.levelMax = gA.level; gA.wantImage = 0; gA.rateSkips = 0;
	gA.rectsMotion = gA.rectsFull = gA.bytesMotion = gA.bytesFull = gA.sharpenRects = gA.sharpenTiles = gA.sharpenBytes = gA.settleSum = gA.settleN = gA.settleMax = 0;
	gA.blockPctMax = 0;
}

} // extern "C"

#endif // KVM_ADAPT_ENABLED
