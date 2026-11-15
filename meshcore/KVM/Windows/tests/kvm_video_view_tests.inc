// the NV12 view of the encoder thread through the real glue (kvm_video.cpp, kvm_encthr.cpp in the manual mode, kvm_nv12view.c, kvm_yuv.c) with the fake encoder that is given the view.
// (A) frozen: a complete key frame, the rectangles of a frame + a ring in the others; (B) flat: a key frame that is flat outside the frame, the VIDEO tiles and their rings.
// Included after the other test files; the harness is built with the default of the product (KVM_NV12_FROZEN_DEFAULT=1, the view of (A) on; (B) off), every scenario here sets the variables of the service it needs.

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void) { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (uint32_t)(g_rng >> 16); }
static std::vector<uint8_t> gViewCopy;
static void view_hook(const uint8_t *nv12, size_t len) { gViewCopy.assign(nv12, nv12 + len); }
static void set_view_env(int frozen, int flat, int firstFull)
{
	setenv("MESH_KVM_NV12_FROZEN", frozen ? "1" : "0", 1); setenv("MESH_KVM_IDR_FLAT", flat ? "1" : "0", 1); setenv("MESH_KVM_IDR_FLAT_FIRST_FULL", firstFull ? "1" : "0", 1);
}
static void clear_view_env(void) { unsetenv("MESH_KVM_NV12_FROZEN"); unsetenv("MESH_KVM_IDR_FLAT"); unsetenv("MESH_KVM_IDR_FLAT_FIRST_FULL"); gViewHook = NULL; }
static uint8_t grey_y(int v) { uint8_t y, cb, cr; kvm_yuv_rgb_to_yuv(v, v, v, &y, &cb, &cr); return y; }
// the picture of the capture: bottom-up, 1920 wide; a rectangle of the screen in grey v
static void paint_px(int x0, int y0, int w, int h, int v)
{
	const int pw = TILE_WIDTH_COUNT * 32, ph = TILE_HEIGHT_COUNT * 32;
	for (int y = y0; y < y0 + h && y < ph; y++) { memset(&gPicture[(size_t)(ph - 1 - y) * (size_t)pw * 4 + (size_t)x0 * 4], v, (size_t)w * 4); }
}
static void paint_all(int v) { memset(gPicture.data(), v, gPicture.size()); }
static void paint_tile(int r, int c, int v) { paint_px(c * 32, r * 32, 32, 32, v); }
static uint8_t view_y(int x, int y) { return gViewCopy.empty() ? 0 : gViewCopy[(size_t)y * 1920 + (size_t)x]; }
static int view_tile_y(int r, int c) { return view_y(c * 32 + 16, r * 32 + 16); }
static void frame_after(int ms) { gTestNowMs += (uint32_t)ms; }
// drains the encoder (the manual thread) so that the picture may be changed again
static void drain(void) { while (kvm_encthr_test_step(1) > 0) { kvm_video_service(); } kvm_video_service(); for (int i = 0; i < 50 && kvm_video_busy(); i++) { gTestNowMs += 5; kvm_video_service(); } }

static void test_view_frozen(void)
{
	set_view_env(1, 0, 0); gViewHook = view_hook; gViewFrames = 0;
	begin_session(1920, 1080); settle_all();
	paint_all(100);
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(5, 10, 5, 10, 600), 1); CHECK_EQ(emit(), 1); drain();
	CHECK_EQ(gViewFrames.load(), 1);
	CHECK_EQ(gViewCopy.size(), (size_t)1920 * 1088 * 3 / 2);
	CHECK(view_y(5, 5) == grey_y(100) && view_y(1900, 1000) == grey_y(100));			// the key frame is complete
	// the picture changes everywhere; only the tile (3, 20) is a motion tile: it and its ring are new, the rest is as it was
	paint_all(200); frame_after(40);
	CHECK_EQ(move_rect(3, 20, 3, 20, 601), 1); CHECK_EQ(emit(), 1); drain();
	CHECK_EQ(view_tile_y(3, 20), grey_y(200));
	CHECK(view_y(20 * 32 - 8, 3 * 32 + 16) == grey_y(200) && view_y(20 * 32 + 40, 3 * 32 + 16) == grey_y(200));	// the ring: 8 px outside
	CHECK(view_y(20 * 32 - 24, 3 * 32 + 16) == grey_y(100) && view_y(20 * 32 + 56, 3 * 32 + 16) == grey_y(100));	// beyond it: frozen
	CHECK(view_y(1900, 1000) == grey_y(100) && view_y(5, 5) == grey_y(100) && view_tile_y(5, 10) == grey_y(100));
	// the tile moves again: fresh again
	paint_tile(3, 20, 160); frame_after(40);
	CHECK_EQ(move_rect(3, 20, 3, 20, 602), 1); CHECK_EQ(emit(), 1); drain();
	CHECK_EQ(view_tile_y(3, 20), grey_y(160));
	// a key frame on request (op 3): complete, the whole picture is the picture of the moment
	frame_after(1100);
	send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_IDR);
	CHECK_EQ(move_rect(8, 8, 8, 8, 603), 1); CHECK_EQ(emit(), 1); drain();
	CHECK(frames().back().f.flags & KVM_FRAME_IDR);
	CHECK(view_y(1900, 1000) == grey_y(200) && view_y(5, 5) == grey_y(200) && view_tile_y(3, 20) == grey_y(160));
	// a frame that is given up (the video stops and starts again): the next one is complete
	paint_all(120); frame_after(100);
	send_ctrl(KVM_OP_VIDEO_OFF, 0); frame_after(2500); kvm_video_poll(); send_ctrl(KVM_OP_VIDEO_ON, 7); kvm_video_poll(); kvm_video_poll(); frame_after(100);
	CHECK_EQ(move_rect(2, 2, 2, 2, 604), 1); CHECK_EQ(emit(), 1); drain();
	CHECK(view_y(1900, 1000) == grey_y(120) && view_y(5, 5) == grey_y(120));
	// another size: a new encoder, a new view of the new size (complete)
	set_geometry(1280, 720); paint_all(90); frame_after(3000); kvm_video_poll(); kvm_video_poll();
	CHECK_EQ(move_rect(2, 2, 2, 2, 605), 1); CHECK_EQ(emit(), 1); drain();
	CHECK_EQ(gViewCopy.size(), (size_t)1280 * 736 * 3 / 2);
	CHECK(gViewCopy[(size_t)700 * 1280 + 1200] == grey_y(90) && gViewCopy[10] == grey_y(90));
	end_session();
	CHECK(log_has("video view mode=frozen"));
	CHECK(!log_has("virgin_mb=1") && log_has("virgin_mb=0"));
	clear_view_env();
	kvm_video_leaked_free(); kvm_fbuf_free_all();
}

static void test_view_flat(void)
{
	// B, the first key frame flat: the background is the dominant colour, the tiles of the frame are real
	set_view_env(1, 1, 0); gViewHook = view_hook; gViewFrames = 0;
	begin_session(1920, 1080); settle_all(); gPadBytes = 20000;					// (the fake access units are padded: a key frame of 20 KB for the tile and its ring)
	paint_all(100); paint_px(1500, 0, 420, 1088, 200); paint_tile(5, 10, 30);
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(5, 10, 5, 10, 600), 1);
	emit_only();
	CHECK(gS.reservedBytes > 0u && gS.reservedBytes < 20000u);							// the window is reserved for a key frame of this area (a tile and its ring), not for the 200 KB of the whole picture
	drain();
	CHECK(frames().back().f.flags & KVM_FRAME_IDR);
	CHECK_EQ(view_tile_y(5, 10), grey_y(30));
	CHECK(view_y(1700, 500) == grey_y(100) && view_y(5, 5) == grey_y(100) && view_y(1900, 1000) == grey_y(100));	// flat: the colour that most of the screen has, not the band of 200
	CHECK(log_has("video idr view real_mbs=") && log_has("complete=0 flat=1"));
	gPadBytes = 0;
	// a P frame of another tile: converted fresh with its ring (virgin macroblocks are counted); the QP detector does not see this frame, the next one of the same tile it does
	const unsigned pf0 = kvm_video_test_qpdet_pframes();
	gPadBytes = 3000;
	const unsigned wb0 = kvm_video_test_rc_win_bytes();
	paint_tile(20, 3, 60); frame_after(40);
	CHECK_EQ(move_rect(20, 3, 20, 3, 601), 1);
	emit_only();
	const unsigned reservedVirgin = gS.reservedBytes;
	drain();
	CHECK_EQ(view_tile_y(20, 3), grey_y(60));
	CHECK(view_y(1700, 500) == grey_y(100));												// still flat outside
	CHECK_EQ(kvm_video_test_qpdet_pframes(), pf0);										// virgin: not an event of the detector
	CHECK_EQ(kvm_video_test_rc_win_bytes(), wb0);										// ... and not the content getting busier for the regulator (its 3 KB are the intra size of the new tile)
	paint_tile(20, 3, 64); frame_after(40);
	CHECK_EQ(move_rect(20, 3, 20, 3, 611), 1);
	emit_only();
	CHECK(reservedVirgin > gS.reservedBytes);											// a frame with virgin macroblocks reserves their intra size on top of an ordinary one
	drain();
	CHECK_EQ(kvm_video_test_qpdet_pframes(), pf0 + 1u);									// the same tile again: an ordinary frame
	CHECK(kvm_video_test_rc_win_bytes() >= wb0 + 2500u);
	gPadBytes = 0;
	// the screen changes (tile (20,3) is 70 now) and a key frame comes: the tiles that the viewer shows from the video (VIDEO) are real in it, the others flat
	paint_tile(20, 3, 70); paint_tile(5, 10, 40); paint_px(0, 1000, 200, 40, 90); frame_after(1100);
	send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_IDR);
	CHECK_EQ(move_rect(8, 8, 8, 8, 602), 1); CHECK_EQ(emit(), 1); drain();
	CHECK(frames().back().f.flags & KVM_FRAME_IDR);
	CHECK_EQ(view_tile_y(20, 3), grey_y(70)); CHECK_EQ(view_tile_y(5, 10), grey_y(40));
	CHECK(view_y(100, 1020) == grey_y(100) && view_y(1700, 500) == grey_y(100));
	// after a repair of the tiles (the VIDEO tiles go back to JPEG) a key frame has nothing to keep: only its own tile is real
	paint_tile(20, 3, 80); frame_after(1100);
	send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_TILES | KVM_REPAIR_IDR);
	frame_after(100);
	CHECK_EQ(move_rect(9, 9, 9, 9, 603), 1); CHECK_EQ(emit(), 1); drain();
	CHECK(view_tile_y(20, 3) == grey_y(100));
	end_session(); gPadBytes = 0;
	CHECK(log_has("video view mode=flat") && !log_has("virgin_mb=0 ") );
	clear_view_env();
	kvm_video_leaked_free(); kvm_fbuf_free_all();
	// firstFull: the first key frame of the session is complete, the second is flat
	set_view_env(1, 1, 1); gViewHook = view_hook;
	begin_session(1920, 1080); settle_all();
	paint_all(100); paint_px(1500, 0, 420, 1088, 200);
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(move_rect(5, 10, 5, 10, 600), 1);
	emit_only();
	CHECK(gS.reservedBytes >= 100000u);													// a complete key frame: the forecast of the whole picture
	drain();
	CHECK(view_y(1700, 500) == grey_y(200));												// complete: the band is in the view
	frame_after(1100);
	send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_TILES | KVM_REPAIR_IDR);
	frame_after(100);
	CHECK_EQ(move_rect(9, 9, 9, 9, 603), 1); CHECK_EQ(emit(), 1); drain();
	CHECK(view_y(1700, 500) == grey_y(100));												// flat
	end_session();
	clear_view_env();
	kvm_video_leaked_free(); kvm_fbuf_free_all();
}

// the invariant of the view under random operations (the manual thread, so that a frame is encoded from the picture of its moment): every tile that a frame paints is converted from the picture of that frame
static void test_view_fuzz(int sessions)
{
	for (int s = 0; s < sessions; s++)
	{
		const int flat = (s % 2) == 1, first = (s % 4) >= 2;
		int ver[40][64];
		set_view_env(1, flat, first); gViewHook = view_hook;
		g_rng = 0x1234567ull + (uint64_t)s * 7919u;
		begin_session(1920, 1080); settle_all();
		paint_all(100);
		send_ctrl(KVM_OP_VIDEO_ON, 7);
		memset(ver, 0, sizeof(ver));
		for (int step = 0; step < 60; step++)
		{
			const int op = (int)(rnd32() % 10);
			const int r = (int)(rnd32() % 34), c = (int)(rnd32() % 60);
			gTestNowMs += 40 + rnd32() % 60;
			if (op == 0) { gTestNowMs += 1100; send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_IDR); }
			else if (op == 1) { gTestNowMs += 1100; send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_TILES | KVM_REPAIR_IDR); }
			else if (op == 2) { kvm_video_note_refresh(); send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_TILES); }
			else if (op == 3 && step > 10) { send_ctrl(KVM_OP_VIDEO_OFF, 0); gTestNowMs += 60; send_ctrl(KVM_OP_VIDEO_ON, 7); }
			const int n = 1 + (int)(rnd32() % 3);
			for (int k = 0; k < n; k++)
			{
				const int rr = (r + k * 3) % 34, cc = (c + k * 5) % 60;
				ver[rr][cc]++;
				paint_tile(rr, cc, 16 + 8 * ((rr * 7 + cc * 3 + ver[rr][cc] * 5) % 28));
				(void)move_rect(rr, cc, rr, cc, 700 + step * 10 + k + s * 1000);
				if (emit() == 1)
				{
					drain();
					// the tile of this claim is in the view with the colour of the picture of this frame
					CHECK_EQ((int)view_tile_y(rr, cc), (int)grey_y(16 + 8 * ((rr * 7 + cc * 3 + ver[rr][cc] * 5) % 28)));
				}
				else { drain(); }
			}
		}
		end_session();
		clear_view_env();
		kvm_video_leaked_free(); kvm_fbuf_free_all();
	}
}

// the hardware backend is given the view too (its table has encode_nv12)
static void test_view_hw(void)
{
	set_view_env(1, 0, 0); gViewHook = view_hook; gViewFrames = 0;
	begin_session(1920, 1080); settle_all(); gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK(log_has("video backend be=hw why=cached"));
	CHECK(hw_frames(5) >= 4);
	CHECK(gViewFrames.load() >= 4); CHECK(gHwEncodes >= 4);
	CHECK(gViewCopy.size() == (size_t)1920 * 1088 * 3 / 2);
	end_session();
	clear_view_env();
	kvm_video_leaked_free(); kvm_fbuf_free_all();
}

static void test_view_all(void)
{
	test_view_hw();
	test_view_frozen();
	test_view_flat();
	test_view_fuzz(24);
}
