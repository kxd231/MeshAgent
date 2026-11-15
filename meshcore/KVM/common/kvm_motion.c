/*
KVM: which tiles are motion while the H.264 video runs, and the diagnostics of the series of motion. See kvm_motion.h. Portable C99, no operating system API.

Licensed under the Apache License, Version 2.0 (same as the rest of the agent).
*/

#include "kvm_motion.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int32_t dms(uint32_t a, uint32_t b) { return (int32_t)(a - b); }

void kvm_motion_init(kvm_motion *m)
{
	memset(m, 0, sizeof(*m));
	m->recentMs = KVM_MOTION_MS; m->recallMs = KVM_MOTION_RECALL_MS; m->streakMin = KVM_MOTION_STREAK; m->tilesMin = KVM_MOTION_TILES; m->radius = KVM_MOTION_NEAR;
}

void kvm_motion_set(kvm_motion *m, uint32_t recentMs, unsigned streakMin, unsigned tilesMin, int radius, uint32_t recallMs)
{
	m->recallMs = recallMs; m->recentMs = recentMs; m->streakMin = streakMin; m->tilesMin = tilesMin; m->radius = radius < 0 ? 0 : radius;
}

void kvm_motion_free(kvm_motion *m)
{
	free(m->at);
	m->at = NULL; m->rows = m->cols = 0;
}

int kvm_motion_resize(kvm_motion *m, int rows, int cols)
{
	if (rows <= 0 || cols <= 0 || (size_t)rows > 4096u || (size_t)cols > 4096u) { kvm_motion_free(m); return -1; }
	if (m->at != NULL && m->rows == rows && m->cols == cols) { memset(m->at, 0, (size_t)rows * (size_t)cols * sizeof(uint32_t)); return 0; }
	free(m->at);
	m->at = (uint32_t *)calloc((size_t)rows * (size_t)cols, sizeof(uint32_t));
	m->rows = rows; m->cols = cols;
	return m->at != NULL ? 0 : -1;
}

void kvm_motion_reset(kvm_motion *m)
{
	if (m->at != NULL) { memset(m->at, 0, (size_t)m->rows * (size_t)m->cols * sizeof(uint32_t)); }
	m->frameAt = 0;
}

void kvm_motion_frame(kvm_motion *m, uint32_t frameAt, unsigned realChanged)
{
	if (realChanged >= m->tilesMin) { m->frameAt = frameAt ? frameAt : 1u; }
}

int kvm_motion_recent(const kvm_motion *m, uint32_t now)
{
	return m->frameAt != 0 && dms(now, m->frameAt) <= (int32_t)m->recentMs;	// a stamp that is later than `now` (the same iteration) is "just now"
}

unsigned kvm_motion_note_change(const kvm_motion *m, unsigned streak, int32_t sinceMs, int sourceChange)
{
	if (sourceChange) { return 0; }	// the picture of another source, not a change of the content
	return (streak > 0 && sinceMs <= (int32_t)m->recentMs) ? (streak < 255u ? streak + 1u : 255u) : 1u;
}

// a tile within `near` tiles of this one that was moving (streak rule) within the last recentMs
static int near_motion(const kvm_motion *m, uint32_t now, int row, int col)
{
	int r, c, r0, r1, c0, c1;
	if (m->at == NULL || m->radius <= 0) { return 0; }
	r0 = row - m->radius; r1 = row + m->radius; c0 = col - m->radius; c1 = col + m->radius;
	if (r0 < 0) { r0 = 0; } if (c0 < 0) { c0 = 0; }
	if (r1 >= m->rows) { r1 = m->rows - 1; } if (c1 >= m->cols) { c1 = m->cols - 1; }
	for (r = r0; r <= r1; r++)
	{
		const uint32_t *line = m->at + (size_t)r * (size_t)m->cols;
		for (c = c0; c <= c1; c++)
		{
			if (line[c] != 0 && dms(now, line[c]) <= (int32_t)m->recentMs) { return 1; }
		}
	}
	return 0;
}

int kvm_motion_class(kvm_motion *m, uint32_t now, int row, int col, unsigned streak, int32_t sinceMs)
{
	if (streak >= m->streakMin && sinceMs <= (int32_t)m->recentMs && kvm_motion_recent(m, now))
	{
		if (m->at != NULL && row >= 0 && col >= 0 && row < m->rows && col < m->cols) { m->at[(size_t)row * (size_t)m->cols + (size_t)col] = now ? now : 1u; }
		m->sStreak++;
		return 1;
	}
	if (near_motion(m, now, row, col)) { m->sNear++; return 1; }
	if (m->recallMs != 0 && m->at != NULL && row >= 0 && col >= 0 && row < m->rows && col < m->cols)
	{
		const uint32_t a = m->at[(size_t)row * (size_t)m->cols + (size_t)col];
		if (a != 0 && dms(now, a) < (int32_t)m->recallMs) { m->sRecall++; return 1; }
	}
	m->sPlain++;
	return 0;
}

void kvm_motion_clear_stats(kvm_motion *m) { m->sStreak = m->sNear = m->sRecall = m->sPlain = 0; }

int kvm_motion_source_absorb(int sourceChange, int crcIsReset) { return (sourceChange && !crcIsReset) ? 1 : 0; }

void kvm_clear_alpha(uint8_t *p, size_t bytes)
{
	size_t i;
	for (i = 0; i + 4 <= bytes; i += 4)
	{
		uint32_t v;
		memcpy(&v, p + i, 4);
		v &= 0x00FFFFFFu;	// (little endian: the agent is x86 / x64 / ARM64 Windows)
		memcpy(p + i, &v, 4);
	}
}

// ---------------------------------------------------------------------------------------------------------------------------------------
// the series of motion
// ---------------------------------------------------------------------------------------------------------------------------------------

static const uint32_t kGapEdge[KVM_GAP_BUCKETS] = { 4, 8, 12, 16, 20, 25, 33, 42, 50, 67, 84, 100, 150, 250, 500, 0xFFFFFFFFu };

static void series_clear_period(kvm_series *s)
{
	memset(s->gap, 0, sizeof(s->gap));
	s->nSeries = s->nStarts = s->noVideo = s->startSum = s->startMax = 0;
	s->rStartMin = 0xFFFFFFFFu;
	s->nGaps = s->gapMax = s->over2x = 0;
	s->paintsJpeg = s->paintsVideo = s->tilesJpeg = 0;
}

void kvm_series_init(kvm_series *s)
{
	memset(s, 0, sizeof(*s));
	series_clear_period(s);
}

static void series_close(kvm_series *s)
{
	if (s->inSeries && !s->haveVideo) { s->noVideo++; }
	s->inSeries = 0; s->haveWrite = 0;
}

void kvm_series_motion(kvm_series *s, uint32_t now, uint32_t rKbps)
{
	if (s->inSeries && dms(now, s->lastMotion) > (int32_t)KVM_SERIES_QUIET_MS) { series_close(s); }
	if (!s->inSeries)
	{
		s->inSeries = 1; s->haveVideo = 0; s->haveWrite = 0; s->seriesStart = now; s->rStartKbps = rKbps;
		s->nSeries++;
		if (rKbps > 0 && rKbps < s->rStartMin) { s->rStartMin = rKbps; }
	}
	s->lastMotion = now;
}

static void gap_note(kvm_series *s, uint32_t g, uint32_t intervalMs)
{
	int i;
	for (i = 0; i < KVM_GAP_BUCKETS - 1; i++) { if (g <= kGapEdge[i]) { break; } }
	s->gap[i]++; s->nGaps++;
	if (g > s->gapMax) { s->gapMax = g; }
	if (intervalMs > 0 && g > 2u * intervalMs) { s->over2x++; }
}

void kvm_series_write(kvm_series *s, uint32_t now, int video, unsigned tiles, uint32_t intervalMs)
{
	if (s->inSeries && dms(now, s->lastMotion) > (int32_t)KVM_SERIES_QUIET_MS) { series_close(s); }
	if (!s->inSeries) { return; }
	if (video) { s->paintsVideo++; } else { s->paintsJpeg++; s->tilesJpeg += tiles; }
	if (video && !s->haveVideo)
	{
		const uint32_t start = (uint32_t)(dms(now, s->seriesStart) < 0 ? 0 : dms(now, s->seriesStart));
		s->haveVideo = 1; s->nStarts++; s->startSum += start;
		if (start > s->startMax) { s->startMax = start; }
	}
	if (s->haveWrite)
	{
		const int32_t g = dms(now, s->lastWrite);
		if (g >= (int32_t)KVM_SERIES_MERGE_MS && g <= (int32_t)KVM_SERIES_GAP_MAX_MS) { gap_note(s, (uint32_t)g, intervalMs); }
	}
	s->haveWrite = 1; s->lastWrite = now;
}

uint32_t kvm_series_gap_p95(const kvm_series *s)
{
	uint32_t acc = 0, need;
	int i;
	if (s->nGaps == 0) { return 0; }
	need = (s->nGaps * 95u + 99u) / 100u;
	for (i = 0; i < KVM_GAP_BUCKETS; i++)
	{
		acc += s->gap[i];
		if (acc >= need) { return (i == KVM_GAP_BUCKETS - 1 || kGapEdge[i] > s->gapMax) ? s->gapMax : kGapEdge[i]; }
	}
	return s->gapMax;
}

size_t kvm_series_format(kvm_series *s, uint32_t now, uint32_t intervalMs, char *buf, size_t cap)
{
	int n;
	uint32_t paints, pct;
	if (s->inSeries && dms(now, s->lastMotion) > (int32_t)KVM_SERIES_QUIET_MS) { series_close(s); }
	paints = s->paintsJpeg + s->paintsVideo;
	if (s->nSeries == 0 && paints == 0) { series_clear_period(s); return 0; }
	pct = paints > 0 ? (s->paintsJpeg * 100u) / paints : 0;
	n = snprintf(buf, cap, "video burst series=%u no_video=%u start_ms_avg=%u start_ms_max=%u r_kbps_start_min=%u paints_jpeg=%u paints_video=%u jpeg_pct=%u tiles_jpeg=%u gaps=%u gap_p95_ms=%u gap_max_ms=%u gap_over2x=%u interval_ms=%u",
				 (unsigned)s->nSeries, (unsigned)s->noVideo, (unsigned)(s->nStarts > 0 ? s->startSum / s->nStarts : 0u), (unsigned)s->startMax, (unsigned)(s->rStartMin == 0xFFFFFFFFu ? 0u : s->rStartMin),
				 (unsigned)s->paintsJpeg, (unsigned)s->paintsVideo, (unsigned)pct, (unsigned)s->tilesJpeg, (unsigned)s->nGaps, (unsigned)kvm_series_gap_p95(s), (unsigned)s->gapMax, (unsigned)s->over2x, (unsigned)intervalMs);
	series_clear_period(s);
	return n > 0 && (size_t)n < cap ? (size_t)n : 0u;
}
