// kvm_h264_check --hw: the part of the hardware encoder probe that needs no Windows API (portable C++03, tested with g++ on Linux): statistics, JSON, the document that the
// watchdog can write at any moment, the synthetic NV12 pictures of the benchmark, and the printing of the rules of kvm_h264_rules.c. The Media Foundation part is kvm_hwprobe_win.cpp.

#ifndef KVM_HWPROBE_CORE_H
#define KVM_HWPROBE_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "../../common/kvm_h264_rules.h"

// ---------------------------------------------------------------------------------------------------------------------- time, lock

double hw_now_ms(void);   // monotonic milliseconds
double hw_cpu_ms(void);   // user + kernel CPU time of the process, milliseconds (0 when unknown)

class HwLock
{
public:
	HwLock();
	~HwLock();
	void lock();
	void unlock();
private:
	void *m_impl;
	HwLock(const HwLock &);
	HwLock &operator=(const HwLock &);
};

struct HwGuard
{
	explicit HwGuard(HwLock &l) : m_l(l) { m_l.lock(); }
	~HwGuard() { m_l.unlock(); }
	HwLock &m_l;
private:
	HwGuard(const HwGuard &);
	HwGuard &operator=(const HwGuard &);
};

// ---------------------------------------------------------------------------------------------------------------------- statistics

struct HwSeries
{
	std::vector<double> v;
	void add(double x) { v.push_back(x); }
	size_t count() const { return v.size(); }
	double sum() const;
	double mean() const;
	double min() const;
	double max() const;
	double pct(double p) const;   // nearest rank, p in 0..1; 0 for an empty series
};

// ---------------------------------------------------------------------------------------------------------------------- JSON

std::string hw_json_str(const char *s);   // "..." with the escapes of RFC 8259 (control characters, quote, backslash; bytes >= 0x80 are copied, or replaced by ? when not valid UTF-8)

class JObj
{
public:
	JObj() : m_first(true) { m_s = "{"; }
	JObj &str(const char *k, const char *v);
	JObj &str(const char *k, const std::string &v) { return str(k, v.c_str()); }
	JObj &i(const char *k, long long v);
	JObj &u(const char *k, unsigned long long v);
	JObj &d(const char *k, double v, int prec = 2);   // NaN / infinity are written as null
	JObj &b(const char *k, bool v);
	JObj &hex(const char *k, unsigned long v);        // "0x%08lX"
	JObj &raw(const char *k, const std::string &json);
	JObj &null(const char *k);
	std::string done() const { return m_s + "}"; }
private:
	void key(const char *k);
	std::string m_s;
	bool m_first;
};

class JArr
{
public:
	JArr() : m_first(true), m_n(0) { m_s = "["; }
	JArr &raw(const std::string &json);
	JArr &i(long long v);
	JArr &str(const char *v);
	size_t size() const { return m_n; }
	std::string done() const { return m_s + "]"; }
private:
	std::string m_s;
	bool m_first;
	size_t m_n;
};

// The one JSON document of a run. Stages add their results as they finish; the watchdog renders whatever exists when a stage hangs. Keys keep the order of their first use.
class HwDoc
{
public:
	HwDoc();
	void set(const char *key, const std::string &json);      // replaces the value of a top level key
	void append(const char *key, const std::string &json);   // adds an element to the array under the key (created on first use)
	std::string render(bool incomplete, const char *note);   // {"kind":"hwprobe","version":1,"incomplete":..,"note":..,<keys>}
private:
	struct Item { std::string key; std::string value; bool isArray; std::vector<std::string> elems; };
	HwLock m_lock;
	std::vector<Item> m_items;
	Item *find(const char *key);
};

// ---------------------------------------------------------------------------------------------------------------------- pictures

enum { HW_CONTENT_TEXT = 0, HW_CONTENT_SCROLL = 1, HW_CONTENT_VIDEO = 2, HW_CONTENT_COUNT = 3 };
const char *hw_content_name(int kind);

// Synthetic desktop pictures straight in NV12 (Y plane, then interleaved UV, stride = width), deterministic.
//   text    a page of text-like glyphs on white with a title bar and a side bar; a caret blinks and a clock field changes every picture (the small updates of a static desktop)
//   scroll  the same page scrolling up by 4 rows per picture (the page repeats after one screen, so the scroll never jumps)
//   video   the same page with a moving plasma and noise field over the middle of the screen (a video player or an animation)
// The size is the coded size (a multiple of 16). Pictures are cheap to make (a copy of rows, a patch of noise), and are made before the timed part of a run or counted separately.
class HwGen
{
public:
	HwGen();
	bool init(int w, int h);
	int width() const { return m_w; }
	int height() const { return m_h; }
	size_t nv12_size() const { return (size_t)m_w * (size_t)m_h * 3 / 2; }
	void frame(int kind, int t, uint8_t *nv12) const;   // picture number t of the kind into nv12 (nv12_size() bytes)
private:
	int m_w, m_h;
	std::vector<uint8_t> m_y, m_uv;   // the page: Y w x 2h, UV w x h
	uint8_t m_sine[256];
	void glyph(int x, int y, int code, int gw, int gh, int lum);
};

// ---------------------------------------------------------------------------------------------------------------------- output

void hw_say(const char *fmt, ...);   // one line of the human summary (stdout, flushed, under a lock; also kept for the end of the report when keep is set below)
std::string hw_hex(const uint8_t *p, size_t n, size_t maxBytes);   // "67 64 00 2A ..." (at most maxBytes)

// The verdicts of a kvmh_check as JSON and as lines of text ("  PASS  annexb       ...").
std::string hw_rules_json(const struct kvmh_check &c);
void hw_rules_print(const struct kvmh_check &c, const char *label);

#endif
