/* KVM: the policy of the hardware H.264 encoder, see kvm_hwpolicy.h. Portable C99. */

#include "kvm_hwpolicy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------------------------------------------------------------------ small helpers

static size_t copy_str(char *dst, size_t cap, const char *src, size_t n)
{
	if (cap == 0) { return 0; }
	if (n >= cap) { n = cap - 1; }
	memcpy(dst, src, n);
	dst[n] = 0;
	return n;
}

static int is_sp(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n'; }

// next token of a line: *p moves behind it; returns its length (0 = none)
static size_t next_tok(const char **p, const char *end, const char **tok)
{
	const char *s = *p;
	while (s < end && is_sp(*s) && *s != '\n') { s++; }
	*tok = s;
	while (s < end && !is_sp(*s)) { s++; }
	*p = s;
	return (size_t)(s - *tok);
}

static int parse_u32(const char *s, size_t n, uint32_t *out)
{
	uint64_t v = 0;
	size_t i;
	if (n == 0 || n > 10) { return 0; }
	for (i = 0; i < n; i++) { if (s[i] < '0' || s[i] > '9') { return 0; } v = v * 10u + (uint64_t)(s[i] - '0'); }
	if (v > 0xFFFFFFFFull) { return 0; }
	*out = (uint32_t)v;
	return 1;
}

// "name=value" token: value pointer and length when the token starts with `name=`
static int tok_val(const char *tok, size_t n, const char *name, const char **val, size_t *vn)
{
	size_t l = strlen(name);
	if (n <= l || memcmp(tok, name, l) != 0 || tok[l] != '=') { return 0; }
	*val = tok + l + 1;
	*vn = n - l - 1;
	return 1;
}

// ------------------------------------------------------------------------------------------------------------------------------ the cache

static void entry_init(kvm_hwp_entry *e, const char *key)
{
	memset(e, 0, sizeof(*e));
	copy_str(e->key, sizeof(e->key), key, strlen(key));
}

kvm_hwp_entry *kvm_hwp_find(kvm_hwp_cache *c, const char *key)
{
	int i;
	for (i = 0; i < c->n; i++) { if (strcmp(c->e[i].key, key) == 0) { return &c->e[i]; } }
	return NULL;
}

kvm_hwp_entry *kvm_hwp_touch(kvm_hwp_cache *c, const char *key, uint32_t nowSec)
{
	kvm_hwp_entry *e = kvm_hwp_find(c, key);
	if (e == NULL)
	{
		if (c->n >= KVM_HWP_ENTRIES)
		{
			int i, old = 0;
			for (i = 1; i < c->n; i++) { if ((int32_t)(c->e[i].used - c->e[old].used) < 0) { old = i; } }
			memmove(&c->e[old], &c->e[old + 1], (size_t)(c->n - old - 1) * sizeof(c->e[0]));
			c->n--;
		}
		e = &c->e[c->n++];
		entry_init(e, key);
	}
	e->used = nowSec;
	return e;
}

static int parse_errs(kvm_hwp_entry *e, const char *v, size_t n)
{
	size_t i = 0;
	e->nErrs = 0;
	while (i < n && e->nErrs < KVM_HWP_ERR_MAX)
	{
		size_t a = i, b;
		uint32_t at = 0, w = 0;
		while (i < n && v[i] != ',') { i++; }
		b = i;
		{
			size_t k = a;
			while (k < b && v[k] != ':') { k++; }
			if (k >= b || !parse_u32(v + a, k - a, &at) || !parse_u32(v + k + 1, b - k - 1, &w) || w == 0 || w > 9) { return 0; }
		}
		e->errs[e->nErrs].at = at; e->errs[e->nErrs].weight = (uint8_t)w; e->nErrs++;
		if (i < n) { i++; }
	}
	return 1;
}

int kvm_hwp_parse(kvm_hwp_cache *c, const char *text, size_t n)
{
	const char *p = text, *end = text + n;
	int first = 1;
	memset(c, 0, sizeof(*c));
	while (p < end)
	{
		const char *ls = p, *le = p, *q, *tok;
		size_t tl;
		while (le < end && *le != '\n') { le++; }
		p = le < end ? le + 1 : le;
		q = ls;
		tl = next_tok(&q, le, &tok);
		if (tl == 0) { continue; }
		if (first)
		{
			first = 0;
			if (!(tl == 5 && memcmp(tok, "kvmhw", 5) == 0)) { return 0; }	// not our file
			tl = next_tok(&q, le, &tok);
			if (tl != 1 || tok[0] != 0x31) { return 0; }				// another version of the format: empty cache (tests again)
			continue;
		}
		if (tl == 1 && tok[0] == 'e')
		{
			kvm_hwp_entry e;
			uint32_t v = 0;
			int ok = 1, haveV = 0;
			tl = next_tok(&q, le, &tok);
			if (tl == 0 || tl >= KVM_HWP_KEY_MAX) { continue; }
			entry_init(&e, "");
			copy_str(e.key, sizeof(e.key), tok, tl);
			while ((tl = next_tok(&q, le, &tok)) != 0 && ok)
			{
				const char *val; size_t vn;
				if (tok_val(tok, tl, "v", &val, &vn)) { ok = parse_u32(val, vn, &v) && v >= 1 && v <= 6; if (ok) { e.verdict = (uint8_t)v; haveV = 1; } }
				else if (tok_val(tok, tl, "at", &val, &vn)) { ok = parse_u32(val, vn, &e.at); }
				else if (tok_val(tok, tl, "fat", &val, &vn)) { ok = parse_u32(val, vn, &e.failAt); }
				else if (tok_val(tok, tl, "used", &val, &vn)) { ok = parse_u32(val, vn, &e.used); }
				else if (tok_val(tok, tl, "att", &val, &vn)) { ok = parse_u32(val, vn, &v) && v < 100; e.attempts = (uint8_t)v; }
				else if (tok_val(tok, tl, "qp", &val, &vn)) { ok = parse_u32(val, vn, &v) && v < 10; e.qpForm = (uint8_t)v; }
				else if (tok_val(tok, tl, "open", &val, &vn)) { ok = parse_u32(val, vn, &e.openMs); }
				else if (tok_val(tok, tl, "p50", &val, &vn)) { ok = parse_u32(val, vn, &e.p50Us); }
				else if (tok_val(tok, tl, "p95", &val, &vn)) { ok = parse_u32(val, vn, &e.p95Us); }
				else if (tok_val(tok, tl, "idr", &val, &vn)) { ok = parse_u32(val, vn, &e.idrKb); }
				else if (tok_val(tok, tl, "flat", &val, &vn)) { ok = parse_u32(val, vn, &e.flatMilli); }
				else if (tok_val(tok, tl, "err", &val, &vn)) { ok = parse_errs(&e, val, vn); }
				// an unknown field is skipped
			}
			if (ok && haveV && c->n < KVM_HWP_ENTRIES && kvm_hwp_find(c, e.key) == NULL) { c->e[c->n++] = e; }
		}
		else if (tl == 1 && tok[0] == 't')
		{
			uint32_t pid = 0, at = 0, used = 0;
			int havePid = 0;
			char key[KVM_HWP_KEY_MAX];
			key[0] = 0;
			while ((tl = next_tok(&q, le, &tok)) != 0)
			{
				const char *val; size_t vn;
				if (tok_val(tok, tl, "pid", &val, &vn)) { havePid = parse_u32(val, vn, &pid); }
				else if (tok_val(tok, tl, "at", &val, &vn)) { if (!parse_u32(val, vn, &at)) { at = 0; } }
				else if (tok_val(tok, tl, "used", &val, &vn)) { if (!parse_u32(val, vn, &used)) { used = 0; } }
				else if (tok_val(tok, tl, "key", &val, &vn)) { if (vn < sizeof(key)) { copy_str(key, sizeof(key), val, vn); } }
			}
			if (havePid) { c->trying = 1; c->tryPid = pid; c->tryAt = at; c->tryUsed = used != 0 ? 1 : 0; copy_str(c->tryKey, sizeof(c->tryKey), key, strlen(key)); }
		}
	}
	return c->n;
}

size_t kvm_hwp_format(const kvm_hwp_cache *c, char *out, size_t cap)
{
	size_t n = 0;
	int i, k;
	int w = snprintf(out, cap, "kvmhw 1\n");
	if (w < 0 || (size_t)w >= cap) { return 0; }
	n = (size_t)w;
	for (i = 0; i < c->n; i++)
	{
		const kvm_hwp_entry *e = &c->e[i];
		w = snprintf(out + n, cap - n, "e %s v=%u at=%u fat=%u used=%u att=%u qp=%u open=%u p50=%u p95=%u idr=%u flat=%u", e->key, (unsigned)e->verdict, (unsigned)e->at, (unsigned)e->failAt, (unsigned)e->used, (unsigned)e->attempts, (unsigned)e->qpForm,
					 (unsigned)e->openMs, (unsigned)e->p50Us, (unsigned)e->p95Us, (unsigned)e->idrKb, (unsigned)e->flatMilli);
		if (w < 0 || (size_t)w >= cap - n) { return 0; }
		n += (size_t)w;
		if (e->nErrs > 0)
		{
			w = snprintf(out + n, cap - n, " err=");
			if (w < 0 || (size_t)w >= cap - n) { return 0; }
			n += (size_t)w;
			for (k = 0; k < e->nErrs; k++)
			{
				w = snprintf(out + n, cap - n, "%s%u:%u", k > 0 ? "," : "", (unsigned)e->errs[k].at, (unsigned)e->errs[k].weight);
				if (w < 0 || (size_t)w >= cap - n) { return 0; }
				n += (size_t)w;
			}
		}
		w = snprintf(out + n, cap - n, "\n");
		if (w < 0 || (size_t)w >= cap - n) { return 0; }
		n += (size_t)w;
	}
	if (c->trying)
	{
		w = snprintf(out + n, cap - n, "t pid=%u at=%u used=%u key=%s\n", (unsigned)c->tryPid, (unsigned)c->tryAt, (unsigned)(c->tryUsed ? 1 : 0), c->tryKey);
		if (w < 0 || (size_t)w >= cap - n) { return 0; }
		n += (size_t)w;
	}
	return n;
}

unsigned kvm_hwp_error_weight(const kvm_hwp_entry *e, uint32_t nowSec)
{
	unsigned w = 0;
	int i;
	for (i = 0; i < e->nErrs; i++)
	{
		const uint32_t age = nowSec - e->errs[i].at;			// seconds; a time in the future (the clock went back) counts as fresh
		if ((int32_t)age < 0 || age <= KVM_HWP_ERROR_TTL_S) { w += e->errs[i].weight; }
	}
	return w;
}

int kvm_hwp_is_bad(const kvm_hwp_entry *e, uint32_t nowSec)
{
	const uint32_t age = nowSec - e->failAt;
	const int testsCount = (e->attempts >= (unsigned)KVM_HWP_MAX_TESTS && ((int32_t)age < 0 || age <= KVM_HWP_ERROR_TTL_S)) ? 1 : 0;	// failed tests expire like errors
	return (kvm_hwp_error_weight(e, nowSec) >= (unsigned)KVM_HWP_BAD_WEIGHT || testsCount) ? 1 : 0;
}

int kvm_hwp_env_parse(const char *value)
{
	if (value == NULL) { return KVM_HWP_ENV_AUTO; }
	if (strcmp(value, "0") == 0) { return KVM_HWP_ENV_OFF; }
	if (strcmp(value, "force") == 0) { return KVM_HWP_ENV_FORCE; }
	return KVM_HWP_ENV_AUTO;
}

int kvm_hwp_mode_parse(const char *text, size_t n)
{
	size_t i = 0, a;
	if (text == NULL) { return KVM_HWP_MODE_AUTO; }
	while (i < n && is_sp(text[i])) { i++; }
	a = i;
	while (i < n && !is_sp(text[i])) { i++; }
	if (i - a == 2 && memcmp(text + a, "sw", 2) == 0) { return KVM_HWP_MODE_SW; }
	if (i - a == 2 && memcmp(text + a, "hw", 2) == 0) { return KVM_HWP_MODE_HW; }
	return KVM_HWP_MODE_AUTO;
}

int kvm_hwp_capture_parse(const char *text, size_t n)
{
	size_t i = 0, a;
	if (text == NULL) { return 0; }
	while (i < n && is_sp(text[i])) { i++; }
	a = i;
	while (i < n && !is_sp(text[i])) { i++; }
	return (i - a == 3 && memcmp(text + a, "gdi", 3) == 0) ? 1 : 0;
}

static void drop_old_errs(kvm_hwp_entry *e)
{
	// keeps the newest KVM_HWP_ERR_MAX (the array is full: the oldest goes)
	if (e->nErrs >= KVM_HWP_ERR_MAX)
	{
		memmove(&e->errs[0], &e->errs[1], (size_t)(KVM_HWP_ERR_MAX - 1) * sizeof(e->errs[0]));
		e->nErrs = KVM_HWP_ERR_MAX - 1;
	}
}

void kvm_hwp_note_flat(kvm_hwp_cache *c, const char *key, uint32_t flatMilli)
{
	kvm_hwp_entry *e = kvm_hwp_find(c, key);
	if (e != NULL) { e->flatMilli = flatMilli > 4000u ? 4000u : flatMilli; }
}

void kvm_hwp_note_test(kvm_hwp_cache *c, const char *key, int verdict, int qpForm, uint32_t openMs, uint32_t p50Us, uint32_t p95Us, uint32_t idrKb, uint32_t nowSec)
{
	kvm_hwp_entry *e = kvm_hwp_touch(c, key, nowSec);
	e->verdict = (uint8_t)verdict;
	e->at = nowSec;
	if (verdict == KVM_HWP_V_PASS) { e->attempts = 0; }
	else if (verdict == KVM_HWP_V_FAIL || verdict == KVM_HWP_V_CRASH || verdict == KVM_HWP_V_TIMEOUT)
	{
		const uint32_t age = nowSec - e->failAt;
		if (e->attempts > 0 && (int32_t)age >= 0 && age > KVM_HWP_ERROR_TTL_S) { e->attempts = 0; }	// the earlier failures are old: they do not count
		if (e->attempts < 99) { e->attempts++; }
		e->failAt = nowSec;
	}
	e->qpForm = (uint8_t)qpForm; e->openMs = openMs; e->p50Us = p50Us; e->p95Us = p95Us; e->idrKb = idrKb;
}

void kvm_hwp_note_error(kvm_hwp_cache *c, const char *key, int weight, uint32_t nowSec)
{
	kvm_hwp_entry *e = kvm_hwp_touch(c, key, nowSec);
	if (weight < 1) { weight = 1; }
	if (weight > 9) { weight = 9; }
	drop_old_errs(e);
	e->errs[e->nErrs].at = nowSec; e->errs[e->nErrs].weight = (uint8_t)weight; e->nErrs++;
	if (e->verdict == KVM_HWP_V_PASS || e->verdict == KVM_HWP_V_NONE) { e->verdict = KVM_HWP_V_RETEST; e->at = nowSec; }
}

void kvm_hwp_begin_try(kvm_hwp_cache *c, const char *key, uint32_t pid, uint32_t nowSec)
{
	c->trying = 1; c->tryPid = pid; c->tryAt = nowSec; c->tryUsed = 0;
	copy_str(c->tryKey, sizeof(c->tryKey), key, strlen(key));
}

void kvm_hwp_end_try(kvm_hwp_cache *c)
{
	c->trying = 0; c->tryPid = 0; c->tryAt = 0; c->tryUsed = 0; c->tryKey[0] = 0;
}

void kvm_hwp_mark_used(kvm_hwp_cache *c, uint32_t pid)
{
	if (c->trying && c->tryPid == pid) { c->tryUsed = 1; }
}

int kvm_hwp_recover_try(kvm_hwp_cache *c, int alive, uint32_t nowSec)
{
	if (!c->trying || alive) { return 0; }
	if (!c->tryUsed && c->tryKey[0] != 0) { kvm_hwp_note_error(c, c->tryKey, 1, nowSec); }	// nobody resolved it: a crash or a session that was cut short, one error
	kvm_hwp_end_try(c);
	return 1;
}

int kvm_hwp_exit_counts(uint32_t exitCode)
{
	if (exitCode == KVM_HWP_STATUS_CONTROL_C || exitCode == KVM_HWP_STATUS_LOGOFF) { return 0; }
	return (exitCode >= KVM_HWP_CRASH_STATUS || exitCode == KVM_HWP_EXC_EXIT) ? 1 : 0;
}

int kvm_hwp_resolve_try(kvm_hwp_cache *c, uint32_t pid, uint32_t exitCode, uint32_t nowSec)
{
	if (!c->trying || c->tryPid != pid) { return 0; }
	if (kvm_hwp_exit_counts(exitCode) && c->tryKey[0] != 0) { kvm_hwp_note_error(c, c->tryKey, 2, nowSec); }
	kvm_hwp_end_try(c);
	return 1;
}

// ------------------------------------------------------------------------------------------------------------------------------ the decision

void kvm_hwp_decide(const kvm_hwp_in *in, kvm_hwp_out *out)
{
	const kvm_hwp_entry *e = in->entry;
	int needTest = 0;
	out->useHardware = 0; out->startTest = 0; out->why = "";
	if (in->env == KVM_HWP_ENV_OFF) { out->why = "env_off"; return; }
	if (!in->osOk) { out->why = "os"; return; }
	if (in->processDisabled) { out->why = "process_disabled"; return; }
	if (in->mode == KVM_HWP_MODE_SW) { out->why = "mode_sw"; return; }
	if (!in->hwPresent) { out->why = "no_hw"; return; }
	if (!in->sizeOk) { out->why = "size"; return; }
	if (e != NULL && kvm_hwp_is_bad(e, in->nowSec) && in->env != KVM_HWP_ENV_FORCE && in->mode != KVM_HWP_MODE_HW) { out->why = "bad_persisted"; return; }
	if (e == NULL || e->verdict == KVM_HWP_V_NONE || e->verdict == KVM_HWP_V_RETEST) { needTest = 1; }
	else if (e->verdict == KVM_HWP_V_SLOW) { if ((uint32_t)(in->nowSec - e->at) >= KVM_HWP_SLOW_RETEST_S) { needTest = 1; } else { out->why = "slow"; return; } }
	else if (e->verdict != KVM_HWP_V_PASS) { needTest = 1; }	// FAIL, CRASH, TIMEOUT that are not BAD yet: one more try
	if (!needTest)
	{
		// the hardware is not started in the lid-closed state (no active output: GDI); one that already runs stays
		if (!in->displayActive && !in->hwRunning) { out->why = "gdi_no_output"; return; }
		out->useHardware = 1; out->why = "cached";
		return;
	}
	if (in->testRunning) { out->why = "test_running"; return; }
	if (in->testDone) { out->why = "test_done"; return; }
	if (!in->displayActive) { out->why = "gdi_no_output"; return; }
	if (!in->desktopDefault) { out->why = "winlogon"; return; }
	out->startTest = 1; out->why = "test_started";
}

// ------------------------------------------------------------------------------------------------------------------------------ key and result

int kvm_hwp_size_class(unsigned codedW, unsigned codedH)
{
	const unsigned long mbs = (unsigned long)((codedW + 15u) / 16u) * (unsigned long)((codedH + 15u) / 16u);
	if (codedW < 16 || codedH < 16 || mbs > 36864ul) { return 0; }
	if (mbs <= 8160ul) { return 1; }
	if (mbs <= 9120ul) { return 2; }
	if (mbs <= 14400ul) { return 3; }
	return 4;
}

static void put_clean(char *dst, size_t cap, const char *s)
{
	size_t i = 0;
	if (cap == 0) { return; }
	for (; s[i] != 0 && i + 1 < cap; i++)
	{
		const char ch = s[i];
		dst[i] = ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '.' || ch == '_' || ch == ':') ? ch : '_';
	}
	dst[i] = 0;
}

size_t kvm_hwp_key_build(const kvm_hwp_sig *s, char *out, size_t cap)
{
	char name[sizeof(s->name)], clsid[sizeof(s->clsid)], drv[sizeof(s->driver)];
	int w;
	put_clean(name, sizeof(name), s->name);
	put_clean(clsid, sizeof(clsid), s->clsid);
	put_clean(drv, sizeof(drv), s->driver);
	w = snprintf(out, cap, "rev:%d,mft:%s,clsid:%s,ven:%04x,dev:%04x,sub:%08x,arev:%02x,drv:%s,os:%u,tok:%d,sz:%d", s->rev, name, clsid, s->vendor & 0xFFFFu, s->device & 0xFFFFu, s->subsys, s->adapterRev & 0xFFu, drv, s->osBuild,
				 s->tokenClass, s->sizeClass);
	if (w < 0 || (size_t)w >= cap || (size_t)w >= KVM_HWP_KEY_MAX) { if (cap > 0) { out[0] = 0; } return 0; }
	return (size_t)w;
}

size_t kvm_hwp_result_format(const kvm_hwp_result *r, char *out, size_t cap)
{
	char rule[sizeof(r->rule)];
	int w;
	put_clean(rule, sizeof(rule), r->rule);
	w = snprintf(out, cap, "kvmhwtest 1\nrev=%d\nverdict=%d\nrule=%s\nqp=%d\nopen=%u\np50=%u\np95=%u\nidr=%u\nframes=%u\nflat=%u\nkey=%s\nend\n", r->rev, r->verdict, rule[0] != 0 ? rule : "-", r->qpForm, (unsigned)r->openMs,
				 (unsigned)r->p50Us, (unsigned)r->p95Us, (unsigned)r->idrKb, (unsigned)r->frames, (unsigned)r->flatMilli, r->key[0] != 0 ? r->key : "-");
	if (w < 0 || (size_t)w >= cap) { return 0; }
	return (size_t)w;
}

void kvm_hwp_result_parse(kvm_hwp_result *r, const char *text, size_t n)
{
	const char *p = text, *end = text + n;
	int first = 1, seenVerdict = 0;
	memset(r, 0, sizeof(*r));
	while (p < end)
	{
		const char *le = p, *tok, *q;
		size_t tl;
		const char *val; size_t vn;
		uint32_t v;
		while (le < end && *le != '\n') { le++; }
		q = p;
		tl = next_tok(&q, le, &tok);
		p = le < end ? le + 1 : le;
		if (tl == 0) { continue; }
		if (first)
		{
			first = 0;
			if (!(tl == 9 && memcmp(tok, "kvmhwtest", 9) == 0)) { return; }
			continue;
		}
		if (tl == 3 && memcmp(tok, "end", 3) == 0) { r->valid = seenVerdict ? 1 : 0; return; }
		if (tok_val(tok, tl, "rev", &val, &vn)) { if (parse_u32(val, vn, &v)) { r->rev = (int)v; } }
		else if (tok_val(tok, tl, "verdict", &val, &vn)) { if (parse_u32(val, vn, &v) && (v == KVM_HWP_V_PASS || v == KVM_HWP_V_FAIL || v == KVM_HWP_V_SLOW)) { r->verdict = (int)v; seenVerdict = 1; } }
		else if (tok_val(tok, tl, "rule", &val, &vn)) { copy_str(r->rule, sizeof(r->rule), val, vn); }
		else if (tok_val(tok, tl, "qp", &val, &vn)) { if (parse_u32(val, vn, &v) && v < 10) { r->qpForm = (int)v; } }
		else if (tok_val(tok, tl, "open", &val, &vn)) { parse_u32(val, vn, &r->openMs); }
		else if (tok_val(tok, tl, "p50", &val, &vn)) { parse_u32(val, vn, &r->p50Us); }
		else if (tok_val(tok, tl, "p95", &val, &vn)) { parse_u32(val, vn, &r->p95Us); }
		else if (tok_val(tok, tl, "idr", &val, &vn)) { parse_u32(val, vn, &r->idrKb); }
		else if (tok_val(tok, tl, "frames", &val, &vn)) { parse_u32(val, vn, &r->frames); }
		else if (tok_val(tok, tl, "flat", &val, &vn)) { parse_u32(val, vn, &r->flatMilli); }
		else if (tok_val(tok, tl, "key", &val, &vn)) { if (!(vn == 1 && val[0] == '-')) { copy_str(r->key, sizeof(r->key), val, vn); } }
	}
}
