// Tests of kvm_hwprobe_core.cpp (portable part of kvm_h264_check --hw): statistics, JSON, document, synthetic pictures, rules as JSON. Run with run-hwprobe-core-tests.sh (g++ in Docker).
#include "../kvm_hwprobe_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

static size_t diff_bytes(const uint8_t *a, const uint8_t *b, size_t n) { size_t d = 0; for (size_t i = 0; i < n; i++) { if (a[i] != b[i]) { d++; } } return d; }

static void test_series(void)
{
	HwSeries s;
	CHECK(s.mean() == 0 && s.max() == 0 && s.min() == 0 && s.pct(0.5) == 0 && s.count() == 0);
	for (int i = 1; i <= 100; i++) { s.add((double)i); }
	CHECK(s.pct(0.5) == 50 && s.pct(0.95) == 95 && s.pct(1.0) == 100 && s.pct(0.0) == 1 && s.max() == 100 && s.min() == 1 && fabs(s.mean() - 50.5) < 1e-9 && s.sum() == 5050);
	HwSeries one; one.add(7.5);
	CHECK(one.pct(0.5) == 7.5 && one.pct(0.95) == 7.5);
}

static void test_json(void)
{
	CHECK(hw_json_str("a\"b\\c\n\t\x01 end") == "\"a\\\"b\\\\c\\n\\t\\u0001 end\"");
	CHECK(hw_json_str(NULL) == "\"\"");
	CHECK(hw_json_str("caf\xC3\xA9") == "\"caf\xC3\xA9\"");		// valid UTF-8 is kept
	CHECK(hw_json_str("a\xC3") == "\"a?\"");					// cut sequence
	CHECK(hw_json_str("a\xFF" "b") == "\"a?b\"");
	CHECK(hw_json_str("\xED\xA0\x80") == "\"???\"");			// a surrogate is not UTF-8
	JObj o;
	o.str("s", "x").i("n", -5).u("u", 18446744073709551615ull).d("f", 1.0 / 3.0, 3).b("t", true).hex("h", 0x80004005ul).null("z");
	CHECK(o.done() == "{\"s\":\"x\",\"n\":-5,\"u\":18446744073709551615,\"f\":0.333,\"t\":true,\"h\":\"0x80004005\",\"z\":null}");
	JObj nan; nan.d("a", 0.0 / 0.0).d("b", 1e308 * 10.0);
	CHECK(nan.done() == "{\"a\":null,\"b\":null}");
	JArr a; a.i(1).str("two").raw("{\"k\":3}").raw("");
	CHECK(a.done() == "[1,\"two\",{\"k\":3},null]" && a.size() == 4);
	JObj e; CHECK(e.done() == "{}");
	JArr ea; CHECK(ea.done() == "[]");
	JObj nested; nested.raw("a", a.done()).raw("o", o.done());
	CHECK(nested.done().find("\"a\":[1,") != std::string::npos);
}

static void test_doc(const char *path)
{
	HwDoc d;
	d.set("meta", JObj().i("cores", 12).done());
	d.append("runs", JObj().i("n", 1).done());
	d.append("runs", JObj().i("n", 2).done());
	d.set("meta", JObj().i("cores", 16).str("os", "10.0.26200").done());		// replaced, keeps its place
	const std::string r = d.render(false, NULL);
	CHECK(r == "{\"kind\":\"hwprobe\",\"version\":1,\"incomplete\":false,\"meta\":{\"cores\":16,\"os\":\"10.0.26200\"},\"runs\":[{\"n\":1},{\"n\":2}]}");
	const std::string r2 = d.render(true, "stage \"bench\" hung");
	CHECK(r2.find("\"incomplete\":true,\"note\":\"stage \\\"bench\\\" hung\"") != std::string::npos);
	HwDoc empty;
	CHECK(empty.render(true, "") == "{\"kind\":\"hwprobe\",\"version\":1,\"incomplete\":true}");
	if (path != NULL)
	{
		FILE *f = fopen(path, "wb");
		if (f != NULL) { fputs(r2.c_str(), f); fclose(f); }
	}
}

static void test_gen(void)
{
	HwGen g;
	CHECK(!g.init(100, 64) && !g.init(64, 100) && !g.init(0, 0) && !g.init(16384, 64) && !g.init(32, 32));
	CHECK(g.init(1920, 1088) && g.width() == 1920 && g.height() == 1088 && g.nv12_size() == 1920u * 1088u * 3 / 2);
	const size_t n = g.nv12_size(), ysz = 1920u * 1088u;
	std::vector<uint8_t> a(n), b(n), c(n);
	// deterministic
	g.frame(HW_CONTENT_SCROLL, 5, &a[0]); g.frame(HW_CONTENT_SCROLL, 5, &b[0]);
	CHECK(a == b);
	// planes in the range of limited range video; chroma neutral on the white page
	{
		int bad = 0;
		for (int k = 0; k < HW_CONTENT_COUNT; k++) { g.frame(k, 7, &a[0]); for (size_t i = 0; i < ysz; i++) { if (a[i] < 16 || a[i] > 235) { bad++; } } }
		CHECK(bad == 0);
	}
	// the page has text (dark pixels) and a white background
	g.frame(HW_CONTENT_TEXT, 0, &a[0]);
	{
		size_t dark = 0, white = 0;
		for (size_t i = 0; i < ysz; i++) { if (a[i] < 60) { dark++; } if (a[i] == 235) { white++; } }
		CHECK(dark > ysz / 40 && white > ysz / 3);
	}
	// text: successive pictures differ only a little (caret and clock), and the clock changes every picture
	g.frame(HW_CONTENT_TEXT, 1, &b[0]); g.frame(HW_CONTENT_TEXT, 2, &c[0]);
	{
		const size_t d01 = diff_bytes(&a[0], &b[0], n), d12 = diff_bytes(&b[0], &c[0], n);
		CHECK(d01 > 0 && d01 < n / 100 && d12 > 0 && d12 < n / 100);
	}
	// scroll: picture t + 1 is picture t moved up by 4 rows (Y), 2 rows (UV); the page repeats after one screen
	g.frame(HW_CONTENT_SCROLL, 10, &a[0]); g.frame(HW_CONTENT_SCROLL, 11, &b[0]);
	CHECK(memcmp(&a[(size_t)4 * 1920], &b[0], (size_t)(1088 - 4) * 1920) == 0);
	CHECK(memcmp(&a[ysz + (size_t)2 * 1920], &b[ysz], (size_t)(544 - 2) * 1920) == 0);
	g.frame(HW_CONTENT_SCROLL, 0, &a[0]); g.frame(HW_CONTENT_SCROLL, 1088 / 4, &b[0]);	// a full turn
	CHECK(a == b);
	// the seam: the last picture of a turn is followed by the first one without a jump of the content (rows move by 4)
	g.frame(HW_CONTENT_SCROLL, 1088 / 4 - 1, &a[0]); g.frame(HW_CONTENT_SCROLL, 1088 / 4, &b[0]);
	CHECK(memcmp(&a[(size_t)4 * 1920], &b[0], (size_t)(1088 - 4) * 1920) == 0);
	// video: the middle of the screen changes a lot, the border does not
	g.frame(HW_CONTENT_VIDEO, 3, &a[0]); g.frame(HW_CONTENT_VIDEO, 4, &b[0]);
	{
		size_t inner = 0, outer = 0;
		const int x0 = 1920 / 4, y0 = 1088 / 4, pw = 1920 / 2, ph = 1088 / 2;
		for (int y = 0; y < 1088; y++) for (int x = 0; x < 1920; x++)
		{
			const bool in = x >= x0 && x < x0 + pw && y >= y0 && y < y0 + ph;
			if (a[(size_t)y * 1920 + x] != b[(size_t)y * 1920 + x]) { (in ? inner : outer)++; }
		}
		CHECK(inner > (size_t)pw * ph / 2 && outer == 0);
	}
	// other sizes
	HwGen s;
	CHECK(s.init(1920, 1216) && s.nv12_size() == 1920u * 1216u * 3 / 2 && s.init(3840, 2176) && s.init(320, 192));
	{
		std::vector<uint8_t> t(s.nv12_size());
		s.frame(HW_CONTENT_TEXT, 3, &t[0]); s.frame(HW_CONTENT_SCROLL, 3, &t[0]); s.frame(HW_CONTENT_VIDEO, 3, &t[0]);
	}
}

static void test_rules_json(void)
{
	struct kvmh_check c;
	kvmh_init(&c, 1920, 1088, 0);
	static const uint8_t au[] = { 0, 0, 0, 1, 0x65, 0x88, 0x84, 0 };		// an IDR slice without parameter sets
	kvmh_au(&c, au, sizeof(au), 1);
	kvmh_finish(&c);
	const std::string j = hw_rules_json(c);
	CHECK(j.find("\"id\":\"idr_ps\",\"status\":\"FAIL\"") != std::string::npos && j.find("\"access_units\":1") != std::string::npos);
	hw_rules_print(c, "a test");
}

int main(int argc, char **argv)
{
	test_series();
	test_json();
	test_doc(argc > 1 ? argv[1] : NULL);
	test_gen();
	test_rules_json();
	printf("hwprobe_core_test: %d checks, %d failed\n", g_checks, g_fail);
	return g_fail != 0;
}
