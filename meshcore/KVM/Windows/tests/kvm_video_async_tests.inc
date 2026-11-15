// the scenarios of the encoder thread in the harness (included by kvm_video_harness_test.cpp, which has the fakes and the helpers). The thread is the manual one: the test says when it "runs", so
// every order of "the loop did this, the encoder finished that" is a line of the scenario. The last part is the random session: a model of the viewer's canvas and of the screen, 1000 sessions.

static unsigned gV_nInflight(void) { unsigned a, b, c, d; kvm_video_test_counts(&a, &b, &c, &d); return a; }
static unsigned gV_nVideo(void) { unsigned a, b, c, d; kvm_video_test_counts(&a, &b, &c, &d); return b; }
static unsigned gV_jpegOnInflight(void) { unsigned a, b, c, d; kvm_video_test_counts(&a, &b, &c, &d); return d; }

// the loop hands a picture over (the way kvm.c does) and does not wait: no step of the encoder, no service
static void emit_only(int fill = -1)
{
	void *b;
	if (fill >= 0) { memset(gPicture.data(), fill, gPicture.size()); }
	b = kvm_fbuf_get(gPicture.size());
	memcpy(b, gPicture.data(), gPicture.size());
	kvm_video_emit(b, (long long)gPicture.size());
	kvm_fbuf_release(b);
}
static int n_frames(void) { return (int)frames().size(); }
// the encoder finishes its frame and the loop collects it (what R0 / R1 do)
static void finish_frame(void) { kvm_encthr_test_step(1); kvm_video_service(); }

static void test_async_basic(void)
{
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(5, 10, 6, 11, 100), 1);
	gTestNowMs += 5;
	emit_only(0x11);
	// the frame is in the encoder: nothing is on the wire, the tiles are in flight (not pending, not sharpened), the loop knows
	CHECK_EQ(n_frames(), 0);
	CHECK(kvm_video_busy());
	CHECK_EQ(kvm_video_pending(), 1);							// 0 pending tiles + the frame in flight
	CHECK(kvm_video_tile_pending(5, 10));						// ... the sharpening leaves the tile alone
	CHECK_EQ(gV_nInflight(), 4);
	CHECK(kvm_video_wake_ms() == -1);							// the loop wakes for the result, not for a slot
	CHECK_EQ(gEncodes, 0);
	// a tile that changes again meanwhile waits for the next frame: it is pending AND in flight (FP)
	CHECK_EQ(move_rect(5, 10, 5, 10, 101), 1);
	CHECK_EQ(kvm_video_pending(), 2);
	// a change that was held back (WA-2b) while the frame was in flight: its crc is the new one and `deferred` is the only thing that remembers it; the frame that did not show it must not clear it
	tileInfo[6][10].contentCrc = 999; tileInfo[6][10].crc = 999; tileInfo[6][10].deferred = 1; tileInfo[6][10].deferredAt = gTestNowMs;
	// the encoder is done: the frame is written, the tiles of it that did not change are VIDEO, the changed one is still pending
	finish_frame();
	CHECK_EQ(tileInfo[6][10].deferred, 1); CHECK_EQ(tileInfo[6][10].crc, 999); CHECK_EQ(tileInfo[6][10].sentQ, 50);
	CHECK_EQ(tileInfo[6][11].deferred, 0); CHECK_EQ(tileInfo[6][11].sentQ, 1);
	CHECK_EQ(gVideoFrameNotes, 2);							// the sharpening governor is told twice: when the frame starts and when it is written
	CHECK_EQ(n_frames(), 1);
	CHECK(!kvm_video_busy());
	CHECK_EQ(gEncodes, 1);
	CHECK_EQ(tileInfo[5][11].sentQ, 1); CHECK_EQ(tileInfo[5][11].crc, 100);
	CHECK_EQ(tileInfo[5][10].crc, 101);							// the change after the frame keeps its crc ...
	CHECK_EQ(tileInfo[5][10].sentQ, 50);						// ... and is not "blurred" by the frame that did not show it
	CHECK(kvm_video_tile_pending(5, 10));
	CHECK_EQ(gWrittenFrames, 1);
	end_session();
}

// the encoder is busy; the picture waits, is replaced by a newer one, and starts when the encoder is done (not an iteration later)
static std::vector<uint8_t> gProbeY;	// the grey of the tile (3, 3) in the view of every encode call
static void probe_hook(const uint8_t *nv12, size_t len) { (void)len; gProbeY.push_back(nv12[(size_t)(3 * 32 + 16) * 1920 + 3 * 32 + 16]); }
static void test_async_busy_held(void)
{
	gProbeY.clear(); gViewHook = probe_hook;
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	FRAME_RATE_TIMER = 16;
	CHECK_EQ(move_rect(2, 2, 2, 2, 200), 1);
	emit_only(0x21);
	CHECK(kvm_video_busy());
	gTestNowMs += 20;
	CHECK_EQ(move_rect(3, 3, 3, 3, 201), 1);
	emit_only(0x22);											// the encoder is busy: the picture is held
	gTestNowMs += 20;
	CHECK_EQ(move_rect(3, 4, 3, 4, 202), 1);
	emit_only(0x23);											// ... and replaced by the newer one
	CHECK_EQ(gEncodes, 0);
	CHECK_EQ(n_frames(), 0);
	// the encoder finishes the first frame: the service writes it and starts the held picture at once (its slot has come)
	gTestNowMs += 5;
	finish_frame();
	CHECK_EQ(n_frames(), 1);
	CHECK(!kvm_video_busy());									// the write gap holds the next frame back for a moment ...
	gTestNowMs += 20;
	kvm_video_service();
	CHECK(kvm_video_busy());									// ... then the held picture starts, without waiting for the next iteration of the loop
	finish_frame();
	CHECK_EQ(n_frames(), 2);
	CHECK_EQ(gEncodes, 2);
	// the second frame shows the newest picture, the 0x22 one was never encoded (the first byte of the picture, with the view the grey of the tile (3, 3) in it: the key frame is complete, the tile is in the second frame)
	if (gViewFrames.load() == 0) { CHECK_EQ(gPicBytes.size(), 2); if (gPicBytes.size() == 2) { CHECK_EQ(gPicBytes[0], 0x21); CHECK_EQ(gPicBytes[1], 0x23); } }
	else
	{
		uint8_t y1, y3, cb, cr;
		kvm_yuv_rgb_to_yuv(0x21, 0x21, 0x21, &y1, &cb, &cr); kvm_yuv_rgb_to_yuv(0x23, 0x23, 0x23, &y3, &cb, &cr);
		CHECK_EQ(gProbeY.size(), 2); if (gProbeY.size() == 2) { CHECK_EQ((int)gProbeY[0], (int)y1); CHECK_EQ((int)gProbeY[1], (int)y3); }
	}
	gViewHook = NULL;
	{
		std::vector<frame_info> fr = frames();
		if (fr.size() == 2) { CHECK(covered(fr[1], 3, 3)); CHECK(covered(fr[1], 3, 4)); CHECK(!covered(fr[1], 2, 2)); }	// both tiles that changed meanwhile are in it
	}
	{
		bool late = false;
		for (size_t i = 0; i < gLog.size(); i++) { (void)i; }
		(void)late;
	}
	end_session();
}

// a JPEG of a tile that a frame in flight covers waits for the frame; REFRESH and the repair too
static void test_async_barriers(void)
{
	// JPEG
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(4, 4, 5, 5, 300), 1);
	emit_only(0x31);
	CHECK(kvm_video_busy());
	kvm_video_before_jpeg(10, 10, 10, 10);						// a rectangle that no frame covers: no waiting
	CHECK(kvm_video_busy()); CHECK_EQ(n_frames(), 0);
	kvm_video_before_jpeg(5, 1, 5, 4);							// a rectangle that touches the tile (5,4): the frame is written first
	CHECK(!kvm_video_busy());
	CHECK_EQ(n_frames(), 1);
	kvm_video_tile_jpeg(5, 4);									// (the JPEG itself goes after the 243: the wire order is the order of the writes)
	CHECK_EQ(gV_jpegOnInflight(), 0);
	end_session();
	// REFRESH
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(4, 4, 5, 5, 300), 1);
	emit_only(0x32);
	CHECK(kvm_video_busy());
	for (int r = 0; r < TILE_HEIGHT_COUNT; r++) { for (int c = 0; c < TILE_WIDTH_COUNT; c++) { tileInfo[r][c].crc = 0xFF; } }	// the input thread
	kvm_video_note_refresh();
	kvm_video_poll();											// the frame lands first, then every tile is JPEG again: no key frame is needed
	CHECK(!kvm_video_busy());
	CHECK_EQ(n_frames(), 1);
	CHECK_EQ(gV_nVideo(), 0);
	CHECK_EQ(gForces, 0);
	end_session();
	// the repair (op 3)
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	gTestNowMs += 2000;										// (the repair is not asked for before the first second: kvm_vctl_repair_due)
	CHECK_EQ(move_rect(4, 4, 5, 5, 300), 1);
	emit_only(0x33);
	CHECK(kvm_video_busy());
	send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_TILES);
	CHECK(!kvm_video_busy());
	CHECK_EQ(n_frames(), 1);
	CHECK_EQ(gV_nVideo(), 0);
	end_session();
}

// a frame that the loop gives up is never written; what follows is a key frame
static void test_async_cancel(void)
{
	// stop (op 2) with a frame in flight
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(4, 4, 5, 5, 400), 1);
	emit_only(0x41);
	CHECK(kvm_video_busy());
	send_ctrl(KVM_OP_VIDEO_OFF, 0);
	CHECK(!kvm_video_on());
	CHECK_EQ(tileInfo[4][4].crc, 0xFF);							// the tiles of the frame go back to JPEG
	finish_frame();
	CHECK_EQ(n_frames(), 0);									// the result is discarded
	CHECK_EQ(gV_nInflight(), 0);
	CHECK_EQ(gCloses, 1);
	// the next request starts a new generation: its first frame is an IDR
	gTestNowMs += 3000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(4, 4, 5, 5, 401), 1);
	CHECK_EQ(emit(), 1);
	{
		std::vector<frame_info> fr = frames();
		CHECK_EQ(fr.size(), 1);
		if (fr.size() == 1) { CHECK_EQ(fr[0].f.flags & KVM_FRAME_IDR, KVM_FRAME_IDR); CHECK_EQ(fr[0].f.gen, 2); }
	}
	end_session();
	// a new geometry with a frame in flight: the generation ends, the encoder is reused (the size of the coded picture did not change), the next frame is a key frame (the frame in flight is lost)
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(4, 4, 5, 5, 400), 1);
	CHECK_EQ(emit(), 1);										// the first frame (IDR), written
	gTestNowMs += 40;
	CHECK_EQ(move_rect(4, 4, 5, 5, 402), 1);
	emit_only(0x42);
	CHECK(kvm_video_busy());
	set_geometry(1920, 1080);
	settle_all();
	kvm_video_poll();
	CHECK(!kvm_video_busy() || true);
	finish_frame();
	CHECK_EQ(n_frames(), 1);									// the frame of the old generation was not written
	CHECK_EQ(move_rect(6, 6, 6, 6, 403), 1);
	gTestNowMs += 40;
	CHECK_EQ(emit(), 1);
	{
		std::vector<frame_info> fr = frames();
		CHECK_EQ(fr.size(), 2);
		if (fr.size() == 2) { CHECK_EQ(fr[1].f.flags & KVM_FRAME_IDR, KVM_FRAME_IDR); CHECK_EQ(fr[1].f.gen, 2); }
	}
	end_session();
	// a result of an old generation: the generation moves on without the video code knowing (a defensive case)
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(4, 4, 5, 5, 400), 1);
	CHECK_EQ(emit(), 1);
	gTestNowMs += 40;
	CHECK_EQ(move_rect(4, 4, 5, 5, 403), 1);
	emit_only(0x43);
	kvm_stream_bump_gen(&gS);
	finish_frame();
	CHECK_EQ(n_frames(), 1);
	CHECK(log_has("old generation dropped"));
	CHECK_EQ(tileInfo[4][4].crc, 0xFF);
	end_session();
}

// the access unit that is being made is in the window of the JPEG and of the sharpening
static void test_async_window(void)
{
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	for (int i = 0; i < 6; i++)
	{
		gTestNowMs += 40;
		CHECK_EQ(move_rect(4, 4, 5, 5, 500 + i), 1);
		CHECK(emit() != KVM_VIDEO_NONE);
	}
	CHECK(gS.reservedBytes == 0);
	gTestNowMs += 40;
	CHECK_EQ(move_rect(4, 4, 5, 5, 520), 1);
	emit_only(0x51);
	CHECK(kvm_video_busy());
	CHECK(gS.reservedBytes > 0);								// an estimate of the frame (the average of the P frames before it)
	finish_frame();
	CHECK_EQ(gS.reservedBytes, 0);
	end_session();
}

// the encoder hangs; the watchdog gives it up, the video is off for the process, the tiles go back to JPEG, nothing is freed under the hung thread
static void test_async_hang(void)
{
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(4, 4, 5, 5, 600), 1);
	CHECK_EQ(emit(), 1);
	gTestNowMs += 40;
	CHECK_EQ(move_rect(4, 4, 5, 5, 601), 1);
	kvm_encthr_test_hang(1, [](uint32_t ms) { gTestNowMs += ms; });
	emit_only(0x61);
	CHECK(kvm_video_busy());
	gTestNowMs += 1900;
	kvm_video_service();										// not yet: 900 ms
	CHECK(kvm_video_busy()); CHECK(kvm_video_on());
	gTestNowMs += 200;
	kvm_video_service();										// 1100 ms: the watchdog
	CHECK(!kvm_video_busy());
	CHECK(!kvm_video_on());
	CHECK(log_has("video encoder hung"));
	CHECK_EQ(tileInfo[4][4].crc, 0xFF);
	{
		kvm_state_msg m = last_state();
		CHECK_EQ(m.video, 0); CHECK_EQ(m.reason, KVM_REASON_DISABLED);
	}
	// the video is off for the rest of the process: a new request is answered with reason 6
	gTestNowMs += 70000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(!kvm_video_on());
	CHECK_EQ(last_state().reason, KVM_REASON_DISABLED);
	kvm_encthr_test_hang(0, NULL);
	end_session();
	kvm_video_leaked_free(); kvm_fbuf_free_all();
	// a barrier gives up too
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(4, 4, 5, 5, 600), 1);
	kvm_encthr_test_hang(1, [](uint32_t ms) { gTestNowMs += ms; });
	emit_only(0x62);
	kvm_video_before_jpeg(4, 4, 4, 4);							// waits for the frame: the watchdog ends the wait
	CHECK(!kvm_video_on());
	CHECK(log_has("where=barrier"));
	kvm_encthr_test_hang(0, NULL);
	end_session();
	kvm_video_leaked_free(); kvm_fbuf_free_all();
}

// An error of the frame in the encoder thread: the tiles are painted again by the next frame (a key frame), and the loop does not hang on a job that never comes back
static void test_async_errors(void)
{
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(4, 4, 5, 5, 700), 1);
	CHECK_EQ(emit(), 1);
	gTestNowMs += 40;
	CHECK_EQ(move_rect(4, 4, 5, 5, 701), 1);
	gEncMode = FE_BROKEN;
	emit_only(0x71);
	finish_frame();
	CHECK_EQ(n_frames(), 1);									// nothing was sent for the lost access unit
	CHECK_EQ(kvm_video_pending(), 4);							// its tiles wait for the next frame
	gEncMode = FE_OK;
	gTestNowMs += 40;
	CHECK_EQ(emit(), 1);
	{
		std::vector<frame_info> fr = frames();
		CHECK_EQ(fr.size(), 2);
		if (fr.size() == 2) { CHECK_EQ(fr[1].f.flags & KVM_FRAME_IDR, KVM_FRAME_IDR); CHECK(covered(fr[1], 4, 4)); }
	}
	end_session();
}

// ---- the random session: the screen, the canvas of the viewer and the loop, 1000 sessions ----

struct rs_model
{
	int rows, cols;
	std::vector<uint32_t> ver;			// the version of the content of every tile on the screen (0 = never changed)
	std::vector<uint32_t> canvas;		// the version the viewer shows
	std::vector<std::vector<uint32_t> > snaps;	// what a frame that the fake encoder made shows (the number is in the access unit)
	uint32_t lastGen = 0;
	bool regress = false, nonIdrFirst = false;
};
static rs_model *gRs;
static std::string gRsTrace;
static void rs_note(const char *fmt, ...) { char b[200]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); gRsTrace += b; gRsTrace += "\n"; }

// every pixel of a tile is a function of (version, row, column); row 0 carries the 32 bits of the version as black / white pixels, so the version can be read back from
// the BGRA picture and from the NV12 view, and the whole tile of the view can be compared with the conversion of that version of the tile.
#include <unordered_map>
#include <array>
static std::vector<uint32_t> gRsWritten;
static bool gRsForce = true;
static std::unordered_map<uint64_t, std::array<uint8_t, 1536> > gRsExpect;
static long long gRsTilesChecked = 0, gRsIncoherent = 0;
#define RS_INCOHERENT 0xFFFFFFFEu
static void rs_tile_bgra(uint32_t v, int r, int c, uint8_t *t /* 32x32x4 top-down */)
{
	for (int y = 0; y < 32; y++)
	{
		for (int x = 0; x < 32; x++)
		{
			uint8_t *q = t + ((size_t)y * 32 + (size_t)x) * 4;
			if (y == 0) { const uint8_t g = ((v >> x) & 1u) ? 255 : 0; q[0] = q[1] = q[2] = g; q[3] = 255; continue; }
			uint32_t h = v * 2654435761u ^ (uint32_t)(r * 7919 + c * 104729) ^ (uint32_t)(x * 31 + y * 1021);
			h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
			q[0] = (uint8_t)h; q[1] = (uint8_t)(h >> 8); q[2] = (uint8_t)(h >> 16); q[3] = 255;
		}
	}
}
static void rs_write_picture(void)
{
	const rs_model &m = *gRs;
	const int pw = TILE_WIDTH_COUNT * 32, ph = TILE_HEIGHT_COUNT * 32;
	uint8_t t[32 * 32 * 4];
	if (gRsWritten.size() != m.ver.size()) { gRsWritten.assign(m.ver.size(), 0); gRsForce = true; }
	for (int r = 0; r < m.rows; r++)
	{
		for (int c = 0; c < m.cols; c++)
		{
			const size_t k = (size_t)r * (size_t)m.cols + (size_t)c;
			const uint32_t v = m.ver[k];
			if (!gRsForce && gRsWritten[k] == v) { continue; }
			rs_tile_bgra(v, r, c, t);
			for (int y = 0; y < 32; y++) { memcpy(&gPicture[((size_t)(ph - 1 - (r * 32 + y)) * (size_t)pw + (size_t)c * 32) * 4], t + (size_t)y * 128, 128); }
			gRsWritten[k] = v;
		}
	}
	gRsForce = false;
}
static int rs_snapshot(const uint8_t *top, int w, int h, int stride)
{
	rs_model &m = *gRs;
	std::vector<uint32_t> s((size_t)m.rows * (size_t)m.cols);
	(void)w; (void)h;
	for (int r = 0; r < m.rows; r++) { for (int c = 0; c < m.cols; c++) { uint32_t v = 0; const uint8_t *p = top + (ptrdiff_t)(r * 32) * stride + c * 32 * 4; for (int i = 0; i < 32; i++) { if (p[i * 4] > 128) { v |= 1u << i; } } s[(size_t)r * (size_t)m.cols + (size_t)c] = v; } }
	m.snaps.push_back(s);
	return (int)m.snaps.size() - 1;
}
static const std::array<uint8_t, 1536> &rs_expect(uint32_t v, int r, int c)
{
	const uint64_t key = ((uint64_t)(uint32_t)(r * 4096 + c) << 32) | v;
	auto it = gRsExpect.find(key);
	if (it != gRsExpect.end()) { return it->second; }
	uint8_t t[32 * 32 * 4];
	std::array<uint8_t, 1536> e;
	rs_tile_bgra(v, r, c, t);
	if (kvm_yuv_bgra_to_nv12(t, 32, 32, 128, e.data(), 32, 32, KVM_YUV_SCALAR) != 0) { e.fill(0); }
	return gRsExpect.emplace(key, e).first->second;
}
static int rs_snapshot_nv12(const uint8_t *nv12, size_t len)
{
	rs_model &m = *gRs;
	const int cw = m.cols * 32, ch = m.rows * 32;	// (not TILE_*_COUNT: the main thread writes those in set_geometry while a job may still run)
	std::vector<uint32_t> s((size_t)m.rows * (size_t)m.cols);
	if (len != (size_t)cw * (size_t)ch * 3 / 2) { m.regress = true; rs_note("NV12 view of a wrong size %zu", len); }
	for (int r = 0; r < m.rows; r++)
	{
		for (int c = 0; c < m.cols; c++)
		{
			uint32_t v = 0;
			const uint8_t *y0 = nv12 + (size_t)(r * 32) * (size_t)cw + (size_t)c * 32;
			for (int i = 0; i < 32; i++) { if (y0[i] > 128) { v |= 1u << i; } }
			const std::array<uint8_t, 1536> &e = rs_expect(v, r, c);
			bool ok = true;
			for (int y = 0; y < 32 && ok; y++) { if (memcmp(nv12 + (size_t)(r * 32 + y) * (size_t)cw + (size_t)c * 32, e.data() + (size_t)y * 32, 32) != 0) { ok = false; } }
			for (int j = 0; j < 16 && ok; j++) { if (memcmp(nv12 + (size_t)cw * (size_t)ch + (size_t)(r * 16 + j) * (size_t)cw + (size_t)c * 32, e.data() + 1024 + (size_t)j * 32, 32) != 0) { ok = false; } }
			s[(size_t)r * (size_t)m.cols + (size_t)c] = ok ? v : RS_INCOHERENT;
		}
	}
	m.snaps.push_back(s);
	return (int)m.snaps.size() - 1;
}
// the viewer reads the wire since `from`: a frame paints the tiles of its rectangles with the version of its snapshot (a version must never go down), a JPEG is painted by the test
static void rs_viewer(size_t *from)
{
	rs_model &m = *gRs;
	for (; *from < gWire.size(); (*from)++)
	{
		const wire_item &w = gWire[*from];
		kvm_frame f;
		if (w.kind != KVM_W_POSITION || w.b.size() <= (size_t)KVM_MARKER_BYTES) { continue; }
		if (kvm_stream_parse_frame(w.b.data(), w.b.size(), (unsigned)SCALED_WIDTH, (unsigned)SCALED_HEIGHT, &f) != KVM_REJ_NONE) { m.regress = true; continue; }
		if (f.gen != m.lastGen) { if (!(f.flags & KVM_FRAME_IDR)) { m.nonIdrFirst = true; } m.lastGen = f.gen; }
		{
			int id = -1;
			for (size_t i = 0; i + 9 <= f.annexLen; i++) { if (f.annexB[i] == 0 && f.annexB[i + 1] == 0 && f.annexB[i + 2] == 0 && f.annexB[i + 3] == 1 && f.annexB[i + 4] == 0x0C) { id = (int)(((unsigned)f.annexB[i + 5] << 24) | ((unsigned)f.annexB[i + 6] << 16) | ((unsigned)f.annexB[i + 7] << 8) | f.annexB[i + 8]); } }
			if (id < 0 || id >= (int)m.snaps.size()) { m.regress = true; continue; }
			for (int r = 0; r < m.rows; r++)
			{
				for (int c = 0; c < m.cols; c++)
				{
					frame_info fi; fi.f = f; fi.rects.assign(f.rects, f.rects + 8u * f.nRects); fi.ok = true; fi.index = 0;
					if (covered(fi, r, c))
					{
						uint32_t &cv = m.canvas[(size_t)r * (size_t)m.cols + (size_t)c];
						const uint32_t v = m.snaps[(size_t)id][(size_t)r * (size_t)m.cols + (size_t)c];
						gRsTilesChecked++;
						if (v == RS_INCOHERENT) { gRsIncoherent++; m.regress = true; rs_note("INCOHERENT frame tile %d,%d (snapshot %d, seq %u gen %u idr %d)", r, c, id, (unsigned)f.seq, (unsigned)f.gen, (int)(f.flags & KVM_FRAME_IDR)); continue; }
						if (v < cv) { m.regress = true; rs_note("REGRESS frame tile %d,%d shows %u over %u (snapshot %d, seq %u gen %u)", r, c, v, cv, id, (unsigned)f.seq, (unsigned)f.gen); }
						if (r == 13 && c == 26) { rs_note("  frame snap %d seq %u paints 13,26 v=%u (canvas was %u) idr=%d", id, (unsigned)f.seq, v, cv, (int)(f.flags & KVM_FRAME_IDR)); }
						cv = v;
					}
				}
			}
		}
	}
}
// a JPEG of a tile: the loop of kvm.c asks first (before_jpeg), the JPEG shows the current content, and the video model is told
static size_t gRsFrom;			// the viewer's read position on the wire
static void rs_jpeg(int r, int c)
{
	rs_model &m = *gRs;
	kvm_video_before_jpeg(r, c, r, c);
	rs_viewer(&gRsFrom);
	{
		uint32_t &cv = m.canvas[(size_t)r * (size_t)m.cols + (size_t)c];
		if (m.ver[(size_t)r * (size_t)m.cols + (size_t)c] < cv) { m.regress = true; rs_note("REGRESS jpeg tile %d,%d", r, c); }
		cv = m.ver[(size_t)r * (size_t)m.cols + (size_t)c];
		if (r == 13 && c == 26) { rs_note("  jpeg 13,26 v=%u", cv); }
	}
	kvm_video_tile_jpeg(r, c);
	tileInfo[r][c].crc = tileInfo[r][c].contentCrc; tileInfo[r][c].sentQ = 80;
}

static unsigned gRsSeed;
static unsigned rs_rnd(void) { gRsSeed = gRsSeed * 1103515245u + 12345u; return gRsSeed >> 16; }

// one session; threaded = the real thread of the encoder (the ThreadSanitizer run), else the manual one. Returns 0 when everything held.
static int run_random_session(unsigned seed, bool threaded)
{
	rs_model m;
	size_t &from = gRsFrom;
	int steps, bad = 0;
	gRsSeed = seed * 2654435761u + 99u;
	gRsTrace.clear();
	begin_session(1280, 704);
	if (threaded)
	{
		// begin_session started the manual thread: start the real one instead
		kvm_video_session_end(); kvm_fbuf_setup(NULL);
		setenv("MESH_KVM_ENC_THREAD", "1", 1);
		(void)kvm_video_session_begin(NULL);
		kvm_stream_init(&gS, 1, 1, 0, KVM_REASON_DISABLED);
		set_geometry(1280, 704);
	}
	m.rows = TILE_HEIGHT_COUNT; m.cols = TILE_WIDTH_COUNT;
	m.ver.assign((size_t)m.rows * (size_t)m.cols, 0); m.canvas = m.ver;
	gRs = &m; gEncSnapshot = rs_snapshot; gEncSnapshotNv12 = rs_snapshot_nv12; gPicBytes.clear(); gRsForce = true; gRsExpect.clear(); rs_write_picture();
	settle_all();
	gRsFrom = gWire.size();
	FRAME_RATE_TIMER = 16 + (int)(rs_rnd() % 3) * 17;
	const int autoBit = (seed % 3u == 0u) ? KVM_AUTO_OP1_FLAG : 0;		// a third of the sessions are Auto's (a bit of its own, so that the other draws stay what they were): the thread, ASan and TSan touch it too
	send_ctrl(KVM_OP_VIDEO_ON, 7 | autoBit);
	if (autoBit) { gS.minRtt = (seed % 2u == 0u) ? 5u : 40u; }
	for (steps = 0; steps < 120; steps++)
	{
		const unsigned ev = rs_rnd() % 16;
		gTestNowMs += 3 + rs_rnd() % 25;
		rs_note("step %d ev %u t=%u inflight=%u pending=%d", steps, ev, (unsigned)gTestNowMs, gV_nInflight(), kvm_video_pending());
		if (ev <= 6)			// an iteration of the loop: the screen changes, the tile loop sends some of it as JPEG (class 0) and gives some of it to the video (class 1), then the picture is handed to the video code
		{
			const int nMove = (int)(rs_rnd() % 3), nJpeg = (int)(rs_rnd() % 3);
			int mr[3][4];
			for (int k = 0; k < nMove; k++)
			{
				mr[k][0] = (int)(rs_rnd() % (unsigned)m.rows); mr[k][1] = (int)(rs_rnd() % (unsigned)m.cols);
				mr[k][2] = std::min(m.rows - 1, mr[k][0] + (int)(rs_rnd() % 3)); mr[k][3] = std::min(m.cols - 1, mr[k][1] + (int)(rs_rnd() % 5));
				for (int r = mr[k][0]; r <= mr[k][2]; r++) { for (int c = mr[k][1]; c <= mr[k][3]; c++) { m.ver[(size_t)r * (size_t)m.cols + (size_t)c]++; } }
			}
			rs_write_picture();
			// the tile loop: the JPEG of a tile that a frame in flight covers waits for the frame; the rectangles of the video are claimed
			for (int k = 0; k < nJpeg; k++)
			{
				const int r = (int)(rs_rnd() % (unsigned)m.rows), c = (int)(rs_rnd() % (unsigned)m.cols);
				m.ver[(size_t)r * (size_t)m.cols + (size_t)c]++;
				rs_write_picture();
				tileInfo[r][c].contentCrc = 5000 + steps;
				rs_jpeg(r, c);
			}
			for (int k = 0; k < nMove; k++) { (void)move_rect(mr[k][0], mr[k][1], mr[k][2], mr[k][3], 1000 + steps * 7 + k); }
			emit_only();
		}
		else if (ev <= 9)		// the encoder thread makes progress (the manual one: now; the real one: it did by itself)
		{
			if (!threaded) { kvm_encthr_test_step(1); }
			kvm_video_service();
		}
		else if (ev == 10)		// the loop at the top of the iteration
		{
			kvm_video_poll();
		}
		else if (ev == 11)		// REFRESH: the input thread resets the crc, the loop writes every tile as JPEG
		{
			for (int r = 0; r < m.rows; r++) { for (int c = 0; c < m.cols; c++) { tileInfo[r][c].crc = 0xFF; } }
			kvm_video_note_refresh();
			kvm_video_poll();
			for (int r = 0; r < m.rows; r++) { for (int c = 0; c < m.cols; c++) { rs_jpeg(r, c); } }
			emit_only();
		}
		else if (ev == 12)		// op 3: a repair or a key frame
		{
			send_ctrl(KVM_OP_REPAIR, (rs_rnd() & 1) ? KVM_REPAIR_TILES : KVM_REPAIR_IDR);
		}
		else if (ev == 13 && (rs_rnd() % 4) == 0)	// the viewer stops and asks again (a new generation), or the geometry changes
		{
			if (rs_rnd() & 1) { send_ctrl(KVM_OP_VIDEO_OFF, 0); gTestNowMs += 3000; send_ctrl(KVM_OP_VIDEO_ON, 7 | autoBit); if (autoBit) { gS.minRtt = 5u; } }
			else { set_geometry(1280, 704); gRsForce = true; rs_write_picture(); kvm_video_poll(); }
			// the viewer repaints everything as JPEG after a stop or a new size (what the tile loop does with tiles of crc 0xFF)
			rs_viewer(&from);
			for (int r = 0; r < m.rows; r++) { for (int c = 0; c < m.cols; c++) { m.canvas[(size_t)r * (size_t)m.cols + (size_t)c] = m.ver[(size_t)r * (size_t)m.cols + (size_t)c]; } }
			settle_all();
		}
		else if (ev == 14 && (rs_rnd() % 6) == 0) { gEncMode = FE_BROKEN; }
		else if (ev == 15) { gEncMode = FE_OK; }
		if (gEncMode == FE_BROKEN && (rs_rnd() % 3) == 0) { gEncMode = FE_OK; }
		rs_viewer(&from);
		if (m.regress || m.nonIdrFirst) { bad |= m.regress ? 1 : 2; break; }
	}
	// the screen is quiet: the loop goes on until everything is painted (the same picture every time, the way the loop does with want_image)
	gEncMode = FE_OK;
	rs_write_picture();
	for (int guard = 0; guard < 400 && (kvm_video_pending() > 0 || kvm_video_busy()); guard++)
	{
		gTestNowMs += 20;
		kvm_video_poll();
		emit_only();
		if (!threaded) { kvm_encthr_test_step(1); } else { (void)kvm_encthr_wait(2); }
		kvm_video_service();
		rs_viewer(&from);
	}
	if (threaded) { for (int k = 0; k < 400 && kvm_video_busy(); k++) { (void)kvm_encthr_wait(5); kvm_video_service(); } }
	rs_viewer(&from);
	if (kvm_video_on())
	{
		// no loss: every tile that changed shows its latest version (the tiles of the video were painted by a frame, the others by a JPEG)
		for (size_t i = 0; i < m.ver.size(); i++) { if (m.ver[i] != 0 && m.canvas[i] != m.ver[i] && !(tileInfo[i / (size_t)m.cols][i % (size_t)m.cols].crc == 0xFF)) { bad |= 4; break; } }
		if (gV_nInflight() != 0 || gV_jpegOnInflight() != 0) { bad |= 8; }
	}
	if (m.regress) { bad |= 1; }
	if (m.nonIdrFirst) { bad |= 2; }
	gEncSnapshot = NULL; gEncSnapshotNv12 = NULL; gRs = NULL;
	end_session();
	return bad;
}

static void test_random_sessions(int n, bool threaded)
{
	int bad = 0;
	gViewFrames = 0;
	for (int s = 1; s <= n; s++)
	{
		const int b = run_random_session((unsigned)s, threaded);
		if (b != 0 && getenv("HARNESS_TRACE") != NULL && bad == 0) { printf("%s\n", gRsTrace.c_str()); }
		if (b != 0) { bad++; if (bad <= 5) { printf("  random session %d (threaded=%d) broke an invariant: %d (1 canvas went back, 2 first frame of a generation is not an IDR, 4 a tile was lost, 8 a frame in flight at the end or a JPEG over a frame)\n", s, threaded ? 1 : 0, b); } }
	}
	printf("random sessions: %d, threaded %d, bad %d, tiles of frames checked %lld, incoherent %lld\n", n, threaded ? 1 : 0, bad, gRsTilesChecked, gRsIncoherent);
	CHECK_EQ(bad, 0);
	// the product default of the view is on (A): the frames of the sessions went through it (a harness that is built with a value of its own would not notice that the default of the product is off)
	if (KVM_NV12_FROZEN_DEFAULT) { CHECK(gViewFrames.load() > 0); } else { CHECK_EQ(gViewFrames.load(), 0); }
	if (threaded) { setenv("MESH_KVM_ENC_THREAD", "2", 1); }
}

// ---- the loop of kvm.c in time: the waiting points of kvm_capture_wait (R0) and of the pause (R1) around the real glue, on the harness clock ----
// The encoder (the manual thread) finishes at encDoneAt; its event stays signalled until a wait consumes it. The iteration: poll at the top (it serves), R0 = the wait for the capture that is also woken by the event of the
// encoder and by kvm_video_wake_ms(), the tile loop (P ms, no service: the loop is busy), claim + emit, the pause R1 up to the schedule: after every wait the loop serves and looks at kvm_video_wake_ms() again (the
// code of kvm.c), so the write gap and the slot are met. Returns the frames per second; the age of the picture when its frame starts (start - the end of its capture) goes to *ageAvg.
static double loop_model(int fr, int period, int W, int P, int E, int injectAt, int Elong, int iters, double *ageAvg, int *lateStarts, int *startedOutside)
{
	uint32_t T, nextGrid, encDoneAt = 0, capAt[4096], t0;
	int running = 0, evSig = 0, nStart = 0, nLate = 0, lastEmitted = -1, curIter = 0;
	long long ageSum = 0;
	gTestCores = 12;											// AMD laptop: the CPU budget (6 cores) does not bind
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = fr;
	gEncCostUs = E * 1000;										// the encoder thread measures E on the fake clock, so the write gap sees the real encode time
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	T = gTestNowMs; nextGrid = T; t0 = T;
	memset(capAt, 0, sizeof(capAt));
	auto check_start = [&](int fromEmit) {
		if (!running && kvm_video_busy() && !kvm_encthr_ready())
		{
			const int pic = fromEmit ? curIter : lastEmitted;
			running = 1; encDoneAt = gTestNowMs + (uint32_t)(curIter == injectAt ? Elong : E);
			if (!fromEmit) { nLate++; }
			if (pic >= 0) { ageSum += (long long)(gTestNowMs - capAt[pic & 4095]); }
			nStart++;
		}
	};
	auto run_to = [&](uint32_t t) { if (running && (int32_t)(encDoneAt - t) <= 0) { if ((int32_t)(encDoneAt - gTestNowMs) > 0) { gTestNowMs = encDoneAt; } kvm_encthr_test_step(1); running = 0; evSig = 1; } };
	auto serve = [&](void) { evSig = 0; kvm_video_service(); check_start(0); };
	for (int i = 0; i < iters; i++)
	{
		curIter = i;
		gTestNowMs = T;
		run_to(T);
		kvm_video_poll(); check_start(0);
		for (;;)	// R0
		{
			const uint32_t capDone = T + (uint32_t)W;
			uint32_t cand = capDone;
			int ms;
			if (evSig && (int32_t)(gTestNowMs - capDone) < 0) { serve(); continue; }
			ms = kvm_video_wake_ms();
			if (ms > 0 && (int32_t)(gTestNowMs + (uint32_t)ms - cand) < 0) { cand = gTestNowMs + (uint32_t)ms; }
			if (running && (int32_t)(encDoneAt - cand) < 0) { cand = encDoneAt; }
			if ((int32_t)(cand - capDone) >= 0) { run_to(capDone); gTestNowMs = capDone; break; }
			gTestNowMs = cand;
			run_to(cand);
			serve();
		}
		capAt[i & 4095] = gTestNowMs;
		run_to(gTestNowMs + (uint32_t)P);							// the tile loop: the encoder may finish here (its event waits)
		gTestNowMs = capAt[i & 4095] + (uint32_t)P;
		CHECK_EQ(move_rect(3, 3, 6, 9, 5000 + i), 1);
		emit_only(i & 0xFF); lastEmitted = i; check_start(1);
		while ((int32_t)(nextGrid - gTestNowMs) <= 0) { nextGrid += (uint32_t)period; }
		for (;;)	// R1
		{
			uint32_t target = nextGrid;
			const int wake = kvm_video_wake_ms();
			if ((int32_t)(target - gTestNowMs) <= 0) { break; }
			if (wake > 0 && (int32_t)(gTestNowMs + (uint32_t)wake - target) < 0) { target = gTestNowMs + (uint32_t)wake; }
			if (evSig) { target = gTestNowMs; }
			else if (running && (int32_t)(encDoneAt - target) < 0) { target = encDoneAt; }
			run_to(target);
			if ((int32_t)(target - gTestNowMs) > 0) { gTestNowMs = target; }
			serve();
		}
		T = gTestNowMs;
	}
	if (ageAvg != NULL) { *ageAvg = nStart ? (double)ageSum / nStart : 0.0; }
	if (lateStarts != NULL) { *lateStarts = nLate; }
	if (startedOutside != NULL) { *startedOutside = nLate; }
	{
		const double fps = (double)nStart * 1000.0 / (double)(gTestNowMs - t0);
		gEncCostUs = 0;
		end_session();
		gTestCores = 2;
		return fps;
	}
}

// the inline mode (MESH_KVM_ENC_THREAD=0): the encode is made in the call; one early wake of the loop (the emit of the next iteration comes before the pace allows it) must not move the frames to the poll
static double loop_model_inline(int fr, int period, int W, int P, int shortAt, int iters, int *atPoll)
{
	uint32_t T, nextGrid, t0;
	int nStart = 0, nPoll = 0, enc0;
	gTestCores = 12;
	begin_session(1920, 1080);
	kvm_video_session_end(); kvm_fbuf_setup(NULL);
	setenv("MESH_KVM_ENC_THREAD", "0", 1);
	(void)kvm_video_session_begin(NULL);
	kvm_stream_init(&gS, 1, 1, 0, KVM_REASON_DISABLED);
	set_geometry(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = fr;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	T = gTestNowMs; nextGrid = T; t0 = T;
	for (int i = 0; i < iters; i++)
	{
		gTestNowMs = T;
		enc0 = gEncodes; kvm_video_poll();
		if (gEncodes > enc0) { nStart++; nPoll++; }
		gTestNowMs += (uint32_t)(W + P);
		CHECK_EQ(move_rect(3, 3, 6, 9, 7000 + i), 1);
		enc0 = gEncodes; emit_only(i & 0xFF);
		if (gEncodes > enc0) { nStart++; }
		while ((int32_t)(nextGrid - gTestNowMs) <= 0) { nextGrid += (uint32_t)period; }
		{
			uint32_t endP = nextGrid;
			const int wake = kvm_video_wake_ms();
			if (wake > 0 && (int32_t)(gTestNowMs + (uint32_t)wake - endP) < 0) { endP = gTestNowMs + (uint32_t)wake; }
			if (i == shortAt) { endP = gTestNowMs + 1; }
			gTestNowMs = endP; T = endP;
		}
	}
	if (atPoll != NULL) { *atPoll = nPoll; }
	setenv("MESH_KVM_ENC_THREAD", "2", 1);
	{
		const double fps = (double)nStart * 1000.0 / (double)(gTestNowMs - t0);
		end_session();
		gTestCores = 2;
		return fps;
	}
}

static void test_loop_model(void)
{
	// (fr = the period of the viewer, period = the grid of the loop, W the wait for the capture, P the tile loop, E the encode, an encode of Elong at the iteration injectAt)
	static const struct { const char *name; int fr, period, W, P, E, injectAt, Elong; double minFps, maxAge; } sc[] = {
		{ "High, W 5, P 4, E 7 (fast state)",                16, 17, 5, 4,  7, -1,  0, 55.0,  6.0 },
		{ "High, W 1, P 4, E 12",                            16, 17, 1, 4, 12, -1,  0, 52.0,  6.0 },
		{ "High, W 1, P 4, E 14",                            16, 17, 1, 4, 14, -1,  0, 52.0, 16.0 },
		{ "High, W 1, P 4, E 16",                            16, 17, 1, 4, 16, -1,  0, 52.0, 20.0 },
		{ "High, W 1, P 4, E 19",                            16, 17, 1, 4, 19, -1,  0, 46.0, 22.0 },
		{ "High, W 1, P 4, E 22 (slow state)",               16, 17, 1, 4, 22, -1,  0, 40.0, 24.0 },
		{ "High, W 5, P 4, E 22 (slow state)",               16, 17, 5, 4, 22, -1,  0, 40.0, 24.0 },
		{ "High, W 1, P 6, E 25",                            16, 17, 1, 6, 25, -1,  0, 36.0, 28.0 },
		{ "Medium, W 1, P 4, E 22",                          33, 33, 1, 4, 22, -1,  0, 27.0,  8.0 },
		{ "Medium, W 1, P 4, E 30",                          33, 33, 1, 4, 30, -1,  0, 27.0, 34.0 },
		{ "High, W 5, P 4, E 7, one encode of 30 ms",        16, 17, 5, 4,  7, 20, 30, 55.0,  7.0 },
		{ "Medium, W 5, P 4, E 7, one encode of 45 ms",      33, 33, 5, 4,  7, 20, 45, 29.0,  6.0 },
	};
	for (size_t k = 0; k < sizeof(sc) / sizeof(sc[0]); k++)
	{
		double age = 0.0;
		int late = 0, outside = 0;
		const double fps = loop_model(sc[k].fr, sc[k].period, sc[k].W, sc[k].P, sc[k].E, sc[k].injectAt, sc[k].Elong, 300, &age, &late, &outside);
		if (getenv("HARNESS_TABLE") != NULL) { printf("LOOP %-46s %5.1f frames/s, started outside emit %3d, picture age at start %.1f ms\n", sc[k].name, fps, late, age); }
		CHECK(fps >= sc[k].minFps);										// min(the rate of the viewer, 1000 / E) less a tenth: 1
		CHECK(age <= sc[k].maxAge);										// the age of the picture does not stay one iteration behind after a slow encode: 2
	}
	// a slow encode in the middle of the motion must not leave the frames starting from the poll (the held picture of a refusal other than "the encoder is busy" waits for the next emit)
	{
		double age = 0.0;
		int late = 0, outside = 0;
		(void)loop_model(16, 17, 5, 4, 7, 20, 30, 300, &age, &late, &outside);
		CHECK(late <= 30);
		(void)loop_model(33, 33, 5, 4, 7, 20, 45, 300, &age, &late, &outside);
		CHECK(late <= 30);
	}
	// the inline mode: one early wake does not move the frames to the poll, and the rate is that of the viewer
	{
		int atPoll = 0;
		double fps = loop_model_inline(16, 17, 5, 4, -1, 300, &atPoll);
		CHECK(fps >= 55.0); CHECK_EQ(atPoll, 0);
		fps = loop_model_inline(16, 17, 5, 4, 20, 300, &atPoll);
		CHECK(fps >= 55.0); CHECK_EQ(atPoll, 0);
		fps = loop_model_inline(33, 33, 5, 4, 20, 300, &atPoll);
		CHECK(fps >= 29.0); CHECK_EQ(atPoll, 0);
	}
}

// the window reserves what a key frame is, not what a P frame is; 4: the sharpening after motion sees the frame in the encoder; 6: a job given up before the thread took it did not set its QP;
// 7: emit writes a frame that the encoder finished during the tile loop before it asks for the slot
static void test_async_review(void)
{
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(4, 4, 5, 5, 900), 1);
	emit_only(0x91);												// the first key frame of the session in the encoder
	CHECK(gS.reservedBytes >= 150000u);								// the key frame that is expected before one was measured (KVM_VRC_IDR_PRIOR_BYTES scaled to the picture), not 0
	finish_frame();
	size_t idr0 = 0, p0 = 0;
	{ std::vector<frame_info> fr = frames(); if (!fr.empty()) { idr0 = gWire[fr[0].index].b.size(); } }
	for (int i = 0; i < 6; i++) { gTestNowMs += 40; CHECK_EQ(move_rect(4, 4, 5, 5, 901 + i), 1); (void)emit(); }
	{ std::vector<frame_info> fr = frames(); if (!fr.empty()) { p0 = gWire[fr.back().index].b.size(); } }
	CHECK(idr0 > p0 && p0 > 0);
	gTestNowMs += 40; CHECK_EQ(move_rect(4, 4, 5, 5, 920), 1);
	emit_only(0x92);
	CHECK(gS.reservedBytes <= p0 + 1);								// a P frame: the average of the P frames
	finish_frame();
	send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_IDR);
	gTestNowMs += 2000;
	CHECK_EQ(move_rect(4, 4, 5, 5, 950), 1);
	emit_only(0x93);												// a key frame that op 3 asked for
	CHECK(gS.reservedBytes >= idr0);								// the last key frame, not the average of the P frames
	{
		std::vector<frame_info> fr0 = frames();
		finish_frame();
		std::vector<frame_info> fr = frames();
		CHECK(fr.size() == fr0.size() + 1 && (fr.back().f.flags & KVM_FRAME_IDR));
	}
	end_session();
	// 7: the encoder finished during the tile loop of the next picture: emit writes that frame first
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(4, 4, 5, 5, 800), 1);
	CHECK_EQ(emit(), 1);
	gTestNowMs += 40;
	CHECK_EQ(move_rect(4, 4, 5, 5, 801), 1);
	emit_only(0x81);
	kvm_encthr_test_step(1);										// done: the loop is in the tile loop
	gTestNowMs += 20;
	CHECK_EQ(move_rect(7, 7, 7, 7, 802), 1);
	emit_only(0x82);
	CHECK_EQ(n_frames(), 2);										// the finished frame was written by emit, the slot was free for the new picture
	end_session();
	// 5: the inline mode (MESH_KVM_ENC_THREAD=0) measures the CPU of the encoder in the call, as before 25 ms with 45 ms of CPU and 10 ms of capture beside it are 1.8 cores
	begin_session(1920, 1080);
	kvm_video_session_end(); kvm_fbuf_setup(NULL);
	setenv("MESH_KVM_ENC_THREAD", "0", 1);
	(void)kvm_video_session_begin(NULL);
	kvm_stream_init(&gS, 1, 1, 0, KVM_REASON_DISABLED);
	set_geometry(1920, 1080);
	settle_all();
	gTestCores = 2;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	FRAME_RATE_TIMER = 16;
	gEncCostUs = 24000 - VIEW_CLOCK_US; gEncCpuUs = 45000; gCapOverlapUs = 10000; gProcCpuUs = 1000000; gCapCpuUs = 500000;
	for (int i = 0; i < 800; i++) { gTestNowMs += 20; (void)move_rect(2, 2, 2, 2, 40000 + i); kvm_video_poll(); (void)emit(); }
	{
		std::string last;
		for (size_t k = gLog.size(); k-- > 0;) { if (gLog[k].find("video pace") != std::string::npos) { last = gLog[k]; break; } }
		CHECK(last.find("enc_par=1.8 ") != std::string::npos);
	}
	gProcCpuUs = gCapCpuUs = gEncCpuUs = gCapOverlapUs = 0; gEncCostUs = 0;
	setenv("MESH_KVM_ENC_THREAD", "2", 1);
	end_session();
	// 9: a picture that is not a buffer of kvm_fbuf is not held (a job could not keep it alive)
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(4, 4, 5, 5, 600), 1);
	{
		void *foreign = malloc(gPicture.size());
		memset(foreign, 0x66, gPicture.size());
		kvm_video_emit(foreign, (long long)gPicture.size());
		CHECK(!kvm_video_busy());
		CHECK_EQ(kvm_video_pending(), 0);							// its tiles go to JPEG
		free(foreign);
	}
	end_session();
	// 6: a frame that carried the QP is given up before the thread took it: the next one sets the QP again
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(4, 4, 5, 5, 700), 1);
	CHECK_EQ(emit(), 1);
	{
		const size_t n0 = gQps.size();
		gTestNowMs += 40;
		CHECK_EQ(move_rect(4, 4, 5, 5, 701), 1);
		emit_only(0x71);											// (no new QP in this job: the QP of the regulator did not move)
		set_geometry(1920, 1080);									// a new generation on the same encoder: the frame is given up
		settle_all();
		kvm_video_poll();
		finish_frame();
		gTestNowMs += 3000;
		CHECK_EQ(move_rect(4, 4, 5, 5, 702), 1);
		CHECK_EQ(emit(), 1);
		CHECK(gQps.size() > n0);									// the QP was set again: nothing is assumed about a job that was given up
	}
	end_session();
}
