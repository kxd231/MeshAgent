// kvm_h264_check --hw: portable part (statistics, JSON, document, synthetic NV12 pictures, printing of the rules). See kvm_hwprobe_core.h.

#include "kvm_hwprobe_core.h"

#include <algorithm>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

// ---------------------------------------------------------------------------------------------------------------------- time, lock

#ifdef _WIN32
double hw_now_ms(void)
{
	static LARGE_INTEGER freq;
	LARGE_INTEGER c;
	if (freq.QuadPart == 0) { QueryPerformanceFrequency(&freq); }
	QueryPerformanceCounter(&c);
	return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

double hw_cpu_ms(void)
{
	FILETIME c, e, k, u;
	if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) { return 0; }
	return (double)(((ULONGLONG)k.dwHighDateTime << 32 | k.dwLowDateTime) + ((ULONGLONG)u.dwHighDateTime << 32 | u.dwLowDateTime)) / 10000.0;
}

HwLock::HwLock() : m_impl(NULL) { CRITICAL_SECTION *cs = new CRITICAL_SECTION; InitializeCriticalSection(cs); m_impl = cs; }
HwLock::~HwLock() { DeleteCriticalSection((CRITICAL_SECTION *)m_impl); delete (CRITICAL_SECTION *)m_impl; }
void HwLock::lock() { EnterCriticalSection((CRITICAL_SECTION *)m_impl); }
void HwLock::unlock() { LeaveCriticalSection((CRITICAL_SECTION *)m_impl); }
#else
double hw_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

double hw_cpu_ms(void)
{
	struct timespec ts;
	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0) { return 0; }
	return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

HwLock::HwLock() : m_impl(NULL) { pthread_mutex_t *m = new pthread_mutex_t; pthread_mutex_init(m, NULL); m_impl = m; }
HwLock::~HwLock() { pthread_mutex_destroy((pthread_mutex_t *)m_impl); delete (pthread_mutex_t *)m_impl; }
void HwLock::lock() { pthread_mutex_lock((pthread_mutex_t *)m_impl); }
void HwLock::unlock() { pthread_mutex_unlock((pthread_mutex_t *)m_impl); }
#endif

// ---------------------------------------------------------------------------------------------------------------------- statistics

double HwSeries::sum() const { double s = 0; for (size_t i = 0; i < v.size(); i++) { s += v[i]; } return s; }
double HwSeries::mean() const { return v.empty() ? 0 : sum() / (double)v.size(); }
double HwSeries::min() const { if (v.empty()) { return 0; } double m = v[0]; for (size_t i = 1; i < v.size(); i++) { if (v[i] < m) { m = v[i]; } } return m; }
double HwSeries::max() const { if (v.empty()) { return 0; } double m = v[0]; for (size_t i = 1; i < v.size(); i++) { if (v[i] > m) { m = v[i]; } } return m; }

double HwSeries::pct(double p) const
{
	if (v.empty()) { return 0; }
	std::vector<double> c(v);
	std::sort(c.begin(), c.end());
	size_t i = (size_t)ceil(p * (double)c.size());
	if (i > 0) { i--; }
	if (i >= c.size()) { i = c.size() - 1; }
	return c[i];
}

// ---------------------------------------------------------------------------------------------------------------------- JSON

std::string hw_json_str(const char *s)
{
	std::string o = "\"";
	const unsigned char *p = (const unsigned char *)(s != NULL ? s : "");
	while (*p != 0)
	{
		const unsigned char c = *p;
		if (c == '"') { o += "\\\""; p++; }
		else if (c == '\\') { o += "\\\\"; p++; }
		else if (c == '\n') { o += "\\n"; p++; }
		else if (c == '\r') { o += "\\r"; p++; }
		else if (c == '\t') { o += "\\t"; p++; }
		else if (c < 0x20 || c == 0x7F) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); o += b; p++; }
		else if (c < 0x80) { o += (char)c; p++; }
		else
		{
			// a UTF-8 sequence is copied only when it is complete and valid, else one ? for the byte
			int len = (c >= 0xC2 && c <= 0xDF) ? 2 : ((c >= 0xE0 && c <= 0xEF) ? 3 : ((c >= 0xF0 && c <= 0xF4) ? 4 : 0));
			bool ok = len > 0;
			for (int k = 1; ok && k < len; k++) { if ((p[k] & 0xC0) != 0x80) { ok = false; } }
			if (ok && len == 3 && ((c == 0xE0 && p[1] < 0xA0) || (c == 0xED && p[1] >= 0xA0))) { ok = false; }
			if (ok && len == 4 && ((c == 0xF0 && p[1] < 0x90) || (c == 0xF4 && p[1] >= 0x90))) { ok = false; }
			if (ok) { for (int k = 0; k < len; k++) { o += (char)p[k]; } p += len; }
			else { o += '?'; p++; }
		}
	}
	return o + "\"";
}

void JObj::key(const char *k)
{
	if (!m_first) { m_s += ","; }
	m_first = false;
	m_s += hw_json_str(k);
	m_s += ":";
}

JObj &JObj::str(const char *k, const char *v) { key(k); m_s += hw_json_str(v); return *this; }
JObj &JObj::i(const char *k, long long v) { char b[32]; snprintf(b, sizeof(b), "%lld", v); key(k); m_s += b; return *this; }
JObj &JObj::u(const char *k, unsigned long long v) { char b[32]; snprintf(b, sizeof(b), "%llu", v); key(k); m_s += b; return *this; }
JObj &JObj::d(const char *k, double v, int prec)
{
	char b[64];
	key(k);
	if (v != v || v > 1e300 || v < -1e300) { m_s += "null"; return *this; }
	snprintf(b, sizeof(b), "%.*f", prec, v);
	m_s += b;
	return *this;
}
JObj &JObj::b(const char *k, bool v) { key(k); m_s += v ? "true" : "false"; return *this; }
JObj &JObj::hex(const char *k, unsigned long v) { char b[24]; snprintf(b, sizeof(b), "\"0x%08lX\"", v); key(k); m_s += b; return *this; }
JObj &JObj::raw(const char *k, const std::string &json) { key(k); m_s += json.empty() ? "null" : json; return *this; }
JObj &JObj::null(const char *k) { key(k); m_s += "null"; return *this; }

JArr &JArr::raw(const std::string &json) { if (!m_first) { m_s += ","; } m_first = false; m_s += json.empty() ? "null" : json; m_n++; return *this; }
JArr &JArr::i(long long v) { char b[32]; snprintf(b, sizeof(b), "%lld", v); return raw(b); }
JArr &JArr::str(const char *v) { return raw(hw_json_str(v)); }

HwDoc::HwDoc() {}

HwDoc::Item *HwDoc::find(const char *key)
{
	for (size_t i = 0; i < m_items.size(); i++) { if (m_items[i].key == key) { return &m_items[i]; } }
	return NULL;
}

void HwDoc::set(const char *key, const std::string &json)
{
	HwGuard g(m_lock);
	Item *it = find(key);
	if (it == NULL) { Item n; n.key = key; n.isArray = false; m_items.push_back(n); it = &m_items.back(); }
	it->isArray = false;
	it->value = json;
}

void HwDoc::append(const char *key, const std::string &json)
{
	HwGuard g(m_lock);
	Item *it = find(key);
	if (it == NULL) { Item n; n.key = key; n.isArray = true; m_items.push_back(n); it = &m_items.back(); }
	it->isArray = true;
	it->elems.push_back(json);
}

std::string HwDoc::render(bool incomplete, const char *note)
{
	HwGuard g(m_lock);
	JObj o;
	o.str("kind", "hwprobe").i("version", 1).b("incomplete", incomplete);
	if (note != NULL && note[0] != 0) { o.str("note", note); }
	for (size_t i = 0; i < m_items.size(); i++)
	{
		const Item &it = m_items[i];
		if (it.isArray)
		{
			JArr a;
			for (size_t k = 0; k < it.elems.size(); k++) { a.raw(it.elems[k]); }
			o.raw(it.key.c_str(), a.done());
		}
		else { o.raw(it.key.c_str(), it.value); }
	}
	return o.done();
}

// ---------------------------------------------------------------------------------------------------------------------- pictures

const char *hw_content_name(int kind) { return kind == HW_CONTENT_TEXT ? "text" : kind == HW_CONTENT_SCROLL ? "scroll" : kind == HW_CONTENT_VIDEO ? "video" : "?"; }

static uint32_t xs(uint32_t &s) { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }

HwGen::HwGen() : m_w(0), m_h(0)
{
	for (int i = 0; i < 256; i++) { m_sine[i] = (uint8_t)(127.5 + 127.5 * sin(i * 3.14159265358979 * 2 / 256)); }
}

// One glyph: a 5 x 7 pattern chosen by the code, drawn with cells of gw/5 x gh/7 pixels, with a lighter pixel next to every stroke (anti-aliasing of a kind).
void HwGen::glyph(int x, int y, int code, int gw, int gh, int lum)
{
	const int sx = std::max(1, gw / 5), sy = std::max(1, gh / 7);
	uint32_t s = (uint32_t)code * 2654435761u + 12345u;
	const int bg = (lum < 128) ? 235 : 70;
	for (int r = 0; r < 7; r++)
	{
		uint32_t bits = xs(s) & 0x1F;
		if (r == 0 || r == 3 || r == 6) { bits |= (xs(s) & 3) ? 0x0E : 0; }	// horizontal strokes now and then
		for (int c = 0; c < 5; c++)
		{
			if (!((bits >> c) & 1)) { continue; }
			for (int yy = 0; yy < sy; yy++)
			{
				const int py = y + r * sy + yy;
				if (py < 0 || py >= 2 * m_h) { continue; }
				for (int xx = 0; xx < sx; xx++)
				{
					const int px = x + c * sx + xx;
					if (px < 0 || px >= m_w) { continue; }
					m_y[(size_t)py * m_w + px] = (uint8_t)lum;
					// the pixel to the right of the last column of the stroke: a mid tone
					if (xx == sx - 1 && px + 1 < m_w && !((c + 1 < 5) && ((bits >> (c + 1)) & 1)) && m_y[(size_t)py * m_w + px + 1] == bg) { m_y[(size_t)py * m_w + px + 1] = (uint8_t)((lum + bg) / 2); }
				}
			}
		}
	}
}

bool HwGen::init(int w, int h)
{
	if (w < 64 || h < 64 || (w & 15) || (h & 15) || w > 8192 || h > 8192) { return false; }
	m_w = w; m_h = h;
	m_y.assign((size_t)w * (size_t)h * 2, 235);
	m_uv.assign((size_t)w * (size_t)h, 128);
	uint32_t seed = 987654321u;
	const int title = std::max(20, h / 40), side = (w / 7) & ~1, line = std::max(16, h / 48);
	const int gh = line * 7 / 10, gw = std::max(5, gh * 5 / 7);
	// title bar and side bar (flat areas), then the text
	for (int y = 0; y < title; y++) { memset(&m_y[(size_t)y * w], 70, (size_t)w); }
	for (int y = title / 2; y < title / 2 + (title + 1) / 2 && y < h / 2; y++) { for (int x = 0; x < w; x += 2) { m_uv[(size_t)y * w + x] = 150; m_uv[(size_t)y * w + x + 1] = 110; } }
	for (int y = title; y < h; y++) { memset(&m_y[(size_t)y * w], 215, (size_t)side); }
	for (int k = 0; k < 14; k++)
	{
		const int y0 = title + 16 + k * (h / 18), wd = side / 2 + (int)(xs(seed) % (unsigned)(side / 3));
		for (int y = y0; y < y0 + gh && y < h; y++) { memset(&m_y[(size_t)y * w + 12], 60, (size_t)std::min(wd, side - 12)); }
	}
	for (int y = title + 8; y + line < h; y += line)
	{
		int x = side + 16;
		const int right = w - 24 - (int)(xs(seed) % (unsigned)(w / 6));
		while (x < right)
		{
			const int nch = 2 + (int)(xs(seed) % 9);
			const bool coloured = (xs(seed) % 10) == 0;
			const int x0 = x;
			for (int k = 0; k < nch && x + gw + 2 < right; k++) { glyph(x, y, 33 + (int)(xs(seed) % 90), gw, gh, 24); x += gw + 2; }
			if (coloured)
			{
				for (int yy = y / 2; yy < (y + gh + 1) / 2 && yy < h; yy++) { for (int xx = x0 & ~1; xx < x; xx += 2) { m_uv[(size_t)yy * w + xx] = 100; m_uv[(size_t)yy * w + xx + 1] = 165; } }
			}
			x += gw * 2;
		}
	}
	// the page repeats after one screen: Y rows [h, 2h) = [0, h), UV rows [h/2, h) = [0, h/2)
	memcpy(&m_y[(size_t)h * w], &m_y[0], (size_t)h * w);
	memcpy(&m_uv[(size_t)(h / 2) * w], &m_uv[0], (size_t)(h / 2) * w);
	return true;
}

void HwGen::frame(int kind, int t, uint8_t *nv12) const
{
	const int w = m_w, h = m_h;
	const size_t ysz = (size_t)w * h;
	int off = 0;
	if (nv12 == NULL || w == 0) { return; }
	if (kind == HW_CONTENT_SCROLL) { off = (int)(((long long)t * 4) % h); }
	memcpy(nv12, &m_y[(size_t)off * w], ysz);
	memcpy(nv12 + ysz, &m_uv[(size_t)(off / 2) * w], ysz / 2);
	if (kind == HW_CONTENT_TEXT)
	{
		// caret (blinks every 15 pictures) and a clock field in the title bar (changes every picture)
		const int cx = (w / 7) + 20, cy = h / 3, gh = std::max(16, h / 48) * 7 / 10;
		const uint8_t cv = ((t / 15) & 1) ? 24 : 235;
		for (int y = cy; y < cy + gh; y++) { nv12[(size_t)y * w + cx] = cv; nv12[(size_t)y * w + cx + 1] = cv; }
		{
			const int gw = std::max(5, gh * 5 / 7), x0 = w - 12 * (gw + 2) - 8;
			for (int k = 0; k < 8; k++)
			{
				const int code = '0' + ((t + k * 3) % 10);
				uint32_t s = (uint32_t)code * 2654435761u + 12345u;
				const int sx = std::max(1, gw / 5), sy = std::max(1, gh / 7);
				for (int r = 0; r < 7; r++)
				{
					uint32_t bits = xs(s) & 0x1F;
					if (r == 0 || r == 3 || r == 6) { bits |= (xs(s) & 3) ? 0x0E : 0; }
					for (int y = 0; y < sy; y++)
					{
						const int py = 4 + r * sy + y;
						// the field is wiped first, then the digit is drawn (same loop: the wipe covers the cell of this digit)
						for (int c = 0; c < 5; c++) { for (int x = 0; x < sx; x++) { nv12[(size_t)py * w + x0 + k * (gw + 2) + c * sx + x] = ((bits >> c) & 1) ? 235 : 70; } }
					}
				}
			}
		}
	}
	else if (kind == HW_CONTENT_VIDEO)
	{
		// plasma + noise over the middle of the screen
		const int x0 = (w / 4) & ~1, y0 = (h / 4) & ~1, pw = (w / 2) & ~1, ph = (h / 2) & ~1;
		uint32_t seed = (uint32_t)t * 2654435761u + 1;
		for (int y = 0; y < ph; y++)
		{
			uint8_t *row = nv12 + (size_t)(y0 + y) * w + x0;
			const int v2 = m_sine[(y * 3 - t * 2) & 255];
			for (int x = 0; x < pw; x++)
			{
				const int v1 = m_sine[(x * 2 + t * 3) & 255];
				const int n = (int)(xs(seed) & 15) - 8;
				const int v = 40 + (v1 >> 1) + (v2 >> 2) + n;
				row[x] = (uint8_t)(v < 16 ? 16 : (v > 235 ? 235 : v));
			}
		}
		for (int y = 0; y < ph; y += 2)
		{
			uint8_t *row = nv12 + ysz + (size_t)((y0 + y) / 2) * w + x0;
			const int v2 = m_sine[(y * 3 - t * 2) & 255];
			for (int x = 0; x < pw; x += 2)
			{
				const int v1 = m_sine[(x * 2 + t * 3) & 255];
				row[x] = (uint8_t)(128 + ((v1 - 128) >> 2));
				row[x + 1] = (uint8_t)(128 + ((v2 - 128) >> 2));
			}
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------------- output

static HwLock g_sayLock;

void hw_say(const char *fmt, ...)
{
	va_list ap;
	HwGuard g(g_sayLock);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
}

std::string hw_hex(const uint8_t *p, size_t n, size_t maxBytes)
{
	std::string o;
	char b[8];
	for (size_t i = 0; i < n && i < maxBytes; i++) { snprintf(b, sizeof(b), "%s%02X", i ? " " : "", p[i]); o += b; }
	if (n > maxBytes) { o += " ..."; }
	return o;
}

std::string hw_rules_json(const struct kvmh_check &c)
{
	JArr a;
	for (int i = 0; i < KVMH_NRULES; i++)
	{
		JObj o;
		o.str("id", kvmh_rule_id(i)).str("status", kvmh_status_name(c.r[i].status)).u("checked", c.r[i].checks).u("failed", c.r[i].fails).str("detail", c.r[i].first);
		a.raw(o.done());
	}
	JObj o;
	o.u("access_units", c.aus).u("idr", c.idrs).u("sps_changes", c.spsChanges).u("b_slices", c.bSlices).raw("rules", a.done());
	if (c.haveSps)
	{
		JObj s;
		s.i("profile_idc", c.sps.profile).hex("constraint_flags", (unsigned long)c.sps.constraint).i("level_idc", c.sps.level).i("num_ref_frames", c.sps.numRefFrames).i("mbs_w", c.sps.mbsW).i("mbs_h", c.sps.mbsH)
			.i("frame_mbs_only", c.sps.frameMbsOnly).i("cropping", c.sps.cropping).i("chroma_format_idc", c.sps.chromaFormat).i("bit_depth_luma", c.sps.bitDepthLuma)
			.i("sps_id", c.extra.spsId).i("poc_type", c.extra.pocType).b("vui", c.extra.vui != 0).b("bitstream_restriction", c.extra.restriction != 0)
			.i("num_reorder_frames", c.extra.reorder).i("max_dec_frame_buffering", c.extra.buffering);
		o.raw("sps", s.done());
	}
	return o.done();
}

void hw_rules_print(const struct kvmh_check &c, const char *label)
{
	hw_say("  rules of the viewer on %s (%u access units, %u IDR):\n", label, c.aus, c.idrs);
	for (int i = 0; i < KVMH_NRULES; i++)
	{
		const struct kvmh_rule &r = c.r[i];
		hw_say("    %-5s %-9s %s%s%s\n", kvmh_status_name(r.status), kvmh_rule_id(i), kvmh_rule_text(i), r.first[0] ? "  <- " : "", r.first);
	}
	if (c.haveSps)
	{
		hw_say("    SPS: profile_idc %d constraint 0x%02X level_idc %d refs %d size %dx%d macroblocks (%dx%d) poc_type %d vui %d restriction %d reorder %d buffering %d\n", c.sps.profile, c.sps.constraint, c.sps.level, c.sps.numRefFrames,
			c.sps.mbsW, c.sps.mbsH, c.sps.mbsW * 16, c.sps.mbsH * 16, c.extra.pocType, c.extra.vui, c.extra.restriction, c.extra.reorder, c.extra.buffering);
	}
}
