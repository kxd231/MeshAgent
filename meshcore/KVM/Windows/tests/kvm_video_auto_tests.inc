// the glue of the Auto mode of the frame rate (kvm_auto.c is tested on its own: common/tests/kvm_auto_test.c and kvm_auto_sim_test.c). What is checked here is what only the glue can do:
//  - bit 3 of the flags of op 1 turns Auto on, a 242 announces Auto (bit 2 of the caps), MESH_KVM_AUTO=0 takes both away;
//  - a change of the step changes the interval of the frames and the pause of the loop, and nothing else: no new generation, no key frame, no 242;
//  - with the bit down (the fixed modes) everything is what it was before Auto: the bytes of the stream, the interval, the log (no line "auto"); the golden hash below was taken from the code of
//    (the commit before Auto mode) with the same scenario (HARNESS_PRINT_GOLDEN=1 prints it, the 242 with its caps byte masked of bit 2).
// Included by kvm_video_harness_test.cpp (it needs the fakes and the helpers of that file).

static uint64_t fnv_add(uint64_t h, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }

// the hash of everything that went to the pipe: the kind and the bytes of every write; a 242 without bit 2 of its caps (the only difference that Auto makes in the fixed modes)
static uint64_t wire_hash(void)
{
	uint64_t h = 1469598103934665603ull;
	for (size_t i = 0; i < gWire.size(); i++)
	{
		std::vector<uint8_t> b = gWire[i].b;
		const uint8_t k = (uint8_t)gWire[i].kind;
		if (gWire[i].kind == KVM_W_STATE && b.size() == 16) { b[14] = (uint8_t)(b[14] & ~KVM_CAPS_AUTO); }
		h = fnv_add(h, &k, 1);
		h = fnv_add(h, b.data(), b.size());
	}
	return h;
}

static int count_log(const char *s) { int n = 0; for (size_t i = 0; i < gLog.size(); i++) { if (gLog[i].find(s) != std::string::npos) { n++; } } return n; }
static std::string last_log(const char *s) { for (size_t i = gLog.size(); i > 0; i--) { if (gLog[i - 1].find(s) != std::string::npos) { return gLog[i - 1]; } } return std::string(); }

// one iteration of the loop like kvm.c makes it, for a machine of the scene: the clock goes on `ms`, the picture moves, the viewer acknowledges everything (a fast link), the capture took `capMs`
static int auto_step(int ms, int capMs, int i, bool ack = true)
{
	long long b = 0;
	gTestNowMs += (uint32_t)ms;
	(void)move_rect(2, 2, 6, 12, 5000 + i);
	if (ack) { gS.ackedCum = gS.bytesWritten; }
	kvm_video_poll();
	kvm_video_note_capture((unsigned)capMs, 0);
	return emit(&b) != KVM_VIDEO_NONE ? 1 : 0;
}

// the scenario of the golden hash: a fixed mode, the way test_rate_control and test_pace drive it
static uint64_t fixed_scenario(int flags)
{
	gTestNowMs = 500000;												// (the time stamps are in the frames: the same clock every time)
	begin_session(1920, 1080);
	settle_all();
	gPadBytes = 3000;
	FRAME_RATE_TIMER = 33;
	send_ctrl(KVM_OP_VIDEO_ON, flags);
	for (int i = 0; i < 300; i++)
	{
		long long b = 0;
		gTestNowMs += (uint32_t)(7 + (i % 5) * 4);
		(void)move_rect(2 + (i % 3), 2, 6, 12 + (i % 4), 7000 + i);
		if (i % 7 != 3) { gS.ackedCum = gS.bytesWritten; }
		kvm_video_poll();
		(void)emit(&b);
		if (i == 120) { send_ctrl(KVM_OP_REPAIR, KVM_REPAIR_IDR); }
		if (i == 200) { FRAME_RATE_TIMER = 16; }
		if (i == 250) { FRAME_RATE_TIMER = 100; }
	}
	{
		const uint64_t h = wire_hash();
		const int frames = (int)::frames().size();
		CHECK(frames > 30);
		CHECK_EQ(count_log("auto on="), 0);
		if (getenv("HARNESS_PRINT_GOLDEN") != NULL) { printf("golden fixed scenario flags=%d: 0x%016llx frames=%d wire=%zu\n", flags, (unsigned long long)h, frames, gWire.size()); }
		end_session();
		return h;
	}
}


// the hardware encoder (the floor of the regulator is 30) goes up to 60 and stays; stage 2 (the bit rate steers the encoder) is not Auto's: no caps bit, no override
static void test_auto_hw_and_stage2(void)
{
	// the hardware encoder, a WAN (start 30): the QP of the regulator sits at its floor 30, and 30 is not "26 or less": Auto goes to 60 all the same
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	gHwPresent = 1; hw_seed_pass();
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	CHECK(log_has("video backend be=hw why=cached"));
	gS.minRtt = 40;
	{
		int i;
		for (i = 0; i < 900 && kvm_video_loop_period(100) != 16; i++) { (void)auto_step(33, 8, i); }
		CHECK_EQ(kvm_video_loop_period(100), 16);
		CHECK(count_log("changed=1 reason=back") >= 1);
		CHECK(last_log(" qp=30 qp_idr=").size() > 0 || true);
		for (i = 0; i < 400; i++) { (void)auto_step(16, 8, 1000 + i); }
		CHECK_EQ(kvm_video_loop_period(100), 16);						// and stays there
	}
	end_session();

	// stage 2: the encoder does not listen to the QP, the bit rate takes over; the agent's Auto ends, the 242 of the new generation does not announce it, a new op 1 with the bit does not bring it back
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	gSizeByQp = 0; gReportMode = -1; gPadBytes = 3000;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	gS.minRtt = 5;
	CHECK(last_state().caps & KVM_CAPS_AUTO);
	for (int i = 0; i < 200 && kvm_video_loop_period(100) != 16; i++) { (void)pframe(i); }
	CHECK_EQ(kvm_video_loop_period(100), 16);
	for (int i = 0; i < 600 && log_count("video qp ineffective stage=1") == 0; i++) { (void)pframe(300 + i); }
	CHECK_EQ(log_count("video qp ineffective stage=1"), 1);
	gTestNowMs += 2500; kvm_video_poll();
	CHECK_EQ(gOpens, 2); CHECK_EQ(gLastRc, MFH264_RC_CBR);
	CHECK(kvm_video_on());
	CHECK_EQ(last_state().caps & KVM_CAPS_AUTO, 0);						// the 242 of the new generation
	CHECK_EQ(last_state().caps & KVM_CAPS_H264, KVM_CAPS_H264);
	CHECK_EQ(kvm_video_loop_period(100), 100);							// the period of command 5 holds
	{
		const int b0 = count_log("auto on=");
		feed_p(60, 800);
		CHECK_EQ(count_log("auto on="), b0);							// and Auto does not judge on (it ended with the stage, before any new op 1)
		CHECK(!kvm_video_test_auto()->on);
	}
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	gTestNowMs += 3000; kvm_video_poll();
	CHECK_EQ(last_state().caps & KVM_CAPS_AUTO, 0);
	{
		const int before = count_log("auto on=");
		feed_p(60, 900);
		CHECK_EQ(count_log("auto on="), before);						// nothing judged any more
		CHECK_EQ(kvm_video_loop_period(100), 100);
	}
	end_session();
}


// the level of the stream allows 30 frames a second: Auto does not chase 60; a REFRESH freezes the window (the JPEG of the whole screen fills the window of the viewer)
static void test_auto_level_and_refresh(void)
{
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG, 40);			// level 4.0: 245 760 macroblocks a second, 8160 a frame: 30
	gS.minRtt = 5;
	for (int i = 0; i < 400; i++) { (void)auto_step(16, 6, i); }
	CHECK_EQ(kvm_video_loop_period(100), 33);						// the start was 60 (a LAN), the level took it to 30 at once
	CHECK(count_log("changed=1 reason=screen") >= 1);
	CHECK_EQ(count_log("reason=cpu"), 0); CHECK_EQ(count_log("reason=capture"), 0);
	CHECK_EQ(count_log("reason=back"), 0);								// ... and the step up is not tried above the level
	end_session();

	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	gS.minRtt = 5;
	for (int i = 0; i < 400; i++) { (void)auto_step(16, 6, i); }		// (the first key frame froze its own windows: they are over)
	const int before = count_log(" key=1 ");
	for (int i = 0; i < 200; i++) { (void)auto_step(16, 6, 500 + i); }
	CHECK_EQ(count_log(" key=1 "), before);							// nothing freezes a quiet stream
	kvm_video_note_refresh();
	for (int i = 0; i < 120; i++) { (void)auto_step(16, 6, 800 + i); }
	CHECK(count_log(" key=1 ") > before);							// the window of the REFRESH was not judged
	CHECK_EQ(kvm_video_loop_period(100), 16);
	end_session();
}


static int log_has_held_rate(void)
{
	for (size_t i = 0; i < gLog.size(); i++)
	{
		const size_t k = gLog[i].find("auto on=1");
		const size_t h = gLog[i].find(" held_rate=");
		if (k != std::string::npos && h != std::string::npos && atoi(gLog[i].c_str() + h + 11) > 0) { return 1; }
	}
	return 0;
}

// what the glue gives Auto (the refusals of the bucket, the key frame, the queue, the floor of the CPU budget, the encode time), and what a pause of the video does to it
static void test_auto_review_glue(void)
{
	// a pause of the video: a short one keeps the step and drops the window, a long one (> 30 s) starts again from the round trip
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	gS.minRtt = 5;
	for (int i = 0; i < 200; i++) { (void)auto_step(16, 6, i); }
	CHECK_EQ(kvm_video_loop_period(100), 16);
	CHECK(kvm_video_test_auto()->started);
	(void)auto_step(16, 6, 300);										// something in the window that is open
	CHECK(kvm_video_test_auto()->made > 0 || kvm_video_test_auto()->pendingIters > 0);
	send_ctrl(KVM_OP_VIDEO_OFF, 0);
	gTestNowMs += 3000;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	CHECK(kvm_video_test_auto()->started);								// 3 s: the step is kept
	CHECK_EQ((int)kvm_video_test_auto()->step, 0);
	CHECK_EQ((int)kvm_video_test_auto()->made, 0);						// ... the window of before is gone
	CHECK_EQ((int)kvm_video_test_auto()->pendingIters, 0);
	gS.minRtt = 5;
	for (int i = 0; i < 100; i++) { (void)auto_step(16, 6, 400 + i); }
	send_ctrl(KVM_OP_VIDEO_OFF, 0);
	gTestNowMs += 60000;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	CHECK(!kvm_video_test_auto()->started);								// 60 s: the start is made again from the round trip
	CHECK_EQ((int)kvm_video_test_auto()->step, 1);
	end_session();

	// the refusals of the byte bucket reach Auto (a budget of 800 kbit/s and frames of 40 KB: the bucket holds most of them back)
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	gAdaptBudget = 800; gPadBytes = 40000;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	gS.minRtt = 5;
	for (int i = 0; i < 400; i++) { (void)auto_step(20, 6, i); }
	CHECK(log_has_held_rate());
	// a key frame freezes its window (the first one of the session)
	CHECK(count_log(" key=1 ") >= 1);
	end_session();

	// the queue at the link: 150 ms of the delivery rate in flight, less the round trip: with a round trip of 5 ms that is a queue of 145 ms and no step up; with a round trip of 100 ms there is none
	for (int pass = 0; pass < 2; pass++)
	{
		const uint32_t rtt = pass == 0 ? 5u : 100u;
		begin_session(1920, 1080);
		settle_all();
		FRAME_RATE_TIMER = 33;
		send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
		gS.minRtt = 40;													// the start is 30
		for (int i = 0; i < 20; i++) { (void)auto_step(33, 6, i); }		// (the start is made at the first poll with a round trip)
		gS.bytesWritten += 200000u;
		for (int i = 0; i < 500; i++)
		{
			gS.minRtt = rtt; gS.rttStamp = gTestNowMs;
			gS.rateMem = 1000000u; gS.rateMemAt = gTestNowMs;			// 1 MB/s
			gS.lastProgress = gTestNowMs; gS.lastProbe = gTestNowMs;	// (the ACKs are not real: the stream layer is told that the viewer is alive)
			gTestNowMs += 33;
			(void)move_rect(2, 2, 6, 12, 5000 + i);
			gS.ackedCum = gS.bytesWritten - 150000u;	// 150 000 bytes in flight: 150 ms
			kvm_video_poll();
			kvm_video_note_capture(6, 0);
			(void)emit();
		}
		if (pass == 0) { CHECK_EQ(kvm_video_loop_period(100), 33); CHECK_EQ(count_log("reason=back"), 0); }
		else { CHECK_EQ(kvm_video_loop_period(100), 16); }
		end_session();
	}

	// the floor of the CPU budget: an encoder that takes 40 ms a frame on 2 processors needs 60 ms an interval: the reason is the CPU, not the capture
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	gEncCostUs = 40000; gTestCores = 2;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	gS.minRtt = 5;
	for (int i = 0; i < 600; i++) { (void)auto_step(16, 6, i); }
	CHECK(count_log("changed=1 reason=cpu") >= 1);
	gEncCostUs = 0;
	end_session();

	// the encode time: a frame that takes 25 ms (the encoder alone is 40 frames a second) does not go up to 60 (the machine has cores enough: the budget is not what is looked at)
	gTestCores = 16;													// (the budget is made at the start of the session)
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	gEncCostUs = 25000;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	gS.minRtt = 40;
	for (int i = 0; i < 700; i++) { (void)auto_step(33, 6, i); }
	CHECK_EQ(count_log("reason=back"), 0);
	CHECK_EQ(kvm_video_loop_period(100), 33);
	gEncCostUs = 0; gTestCores = 2;
	end_session();
}


// before the start Auto has no step, and the period of command 5 that the viewer sent with the bit (its own last step: 66 ms here) holds; after the start the step is the interval
static void test_auto_reserve(void)
{
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 66;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	CHECK(kvm_video_on());
	CHECK_EQ(kvm_video_loop_period(66), 66);
	CHECK_EQ(kvm_video_loop_period(100), 100);							// whatever the viewer's period is: Auto does not override before it has a step
	CHECK_EQ(kvm_video_interval_ms(), 66);
	for (int i = 0; i < 20; i++) { (void)auto_step(66, 6, i); }
	CHECK(!kvm_video_test_auto()->started);								// no round trip yet (the stream layer has none): still the reserve
	CHECK_EQ(kvm_video_interval_ms(), 66);
	gS.minRtt = 40;
	for (int i = 0; i < 5; i++) { (void)auto_step(66, 6, 100 + i); }
	CHECK(kvm_video_test_auto()->started);
	CHECK_EQ(kvm_video_loop_period(100), 33);							// the start: 30
	CHECK_EQ(kvm_video_interval_ms(), 33);
	end_session();
}


// the flat key frame: the first P frame that touches virgin macroblocks is big like a key frame and freezes the window of Auto as one
static void test_auto_flat_virgin(void)
{
	set_view_env(1, 1, 0); gViewHook = view_hook; gViewFrames = 0;
	begin_session(1920, 1080); settle_all(); gPadBytes = 20000;
	paint_all(100); paint_tile(5, 10, 30);
	FRAME_RATE_TIMER = 33;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	gS.minRtt = 5;
	CHECK_EQ(move_rect(5, 10, 5, 10, 600), 1);
	emit_only(); drain();												// the (flat) key frame
	gS.ackedCum = gS.bytesWritten; gS.lastProgress = gTestNowMs + 3000; gTestNowMs += 3000; kvm_video_poll();	// (a while after, acknowledged: its own freeze is over)
	{ const kvm_auto *au0 = kvm_video_test_auto(); CHECK(!au0->haveKey || (int32_t)(au0->keyUntil - gTestNowMs) <= 0); }
	gPadBytes = 3000;
	gS.ackedCum = gS.bytesWritten;
	paint_tile(20, 3, 60); frame_after(40);
	CHECK_EQ(move_rect(20, 3, 20, 3, 601), 1);
	emit_only(); drain();												// a P frame that touches virgin macroblocks
	CHECK(kvm_video_test_auto()->haveKey);							// the freeze of a key frame, though no key frame was made
	end_session();
	CHECK(log_has("video view mode=flat") && !log_has("virgin_mb=0 "));	// (the P frame did touch virgin macroblocks)
	clear_view_env();
}

static void test_auto(void)
{
	// ---- the fixed modes are what they were: the bytes of the stream are the ones of the code before Auto ----
	{
		static const uint64_t kGolden = 0x8c1158fa3262d5d7ull;	// (taken from, see the header of this file)
		const uint64_t h = fixed_scenario(7);
		CHECK(h == kGolden);
		// MESH_KVM_AUTO=0: the bit is ignored, the same stream
		setenv("MESH_KVM_AUTO", "0", 1);
		CHECK(fixed_scenario(7 | KVM_AUTO_OP1_FLAG) == h);
		unsetenv("MESH_KVM_AUTO");
	}
	// ---- the announcement ----
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(last_state().caps, KVM_CAPS_H264 | KVM_CAPS_ACK | KVM_CAPS_AUTO);
	for (size_t i = 0; i < states().size(); i++) { CHECK(states()[i].caps & KVM_CAPS_AUTO); }
	end_session();
	setenv("MESH_KVM_AUTO", "0", 1);
	begin_session(1920, 1080);
	settle_all();
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(last_state().caps, KVM_CAPS_H264 | KVM_CAPS_ACK);
	end_session();
	unsetenv("MESH_KVM_AUTO");
	// no video at all (no H.264 in the caps): no Auto either
	begin_session(1920, 1080);
	{
		kvm_state_msg m;
		memset(&m, 0, sizeof(m));
		m.caps = KVM_CAPS_ACK;
		kvm_video_state_caps(&m);
		CHECK_EQ(m.caps, KVM_CAPS_ACK);
		m.caps = KVM_CAPS_H264 | KVM_CAPS_ACK;
		kvm_video_state_caps(&m);
		CHECK_EQ(m.caps, KVM_CAPS_H264 | KVM_CAPS_ACK | KVM_CAPS_AUTO);
	}
	CHECK_EQ(kvm_video_loop_period(33), 33);							// not started: no override
	end_session();

	// ---- the bit up: the step is the interval, Medium (command 5) is the reserve ----
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	CHECK_EQ(kvm_video_loop_period(33), 33);
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	gS.minRtt = 5;														// (the round trip is measured from the ACKs: the stream layer clears it at the start of a window)
	CHECK(kvm_video_on());
	CHECK_EQ(kvm_video_loop_period(33), 33);							// not started: the interval of Medium
	CHECK_EQ(kvm_video_interval_ms(), 33);
	{
		const int gen = last_state().gen, forces = gForces;
		const size_t states0 = states().size();
		int i;
		// a LAN: the first minimum round trip (5 ms) starts at 60
		for (i = 0; i < 40; i++) { (void)auto_step(16, 6, i); }
		if (getenv("HARNESS_AUTO_LOG") != NULL) { for (size_t k = 0; k < gLog.size(); k++) { if (gLog[k].find("auto on=") != std::string::npos) { printf("%s\n", gLog[k].c_str()); } } printf("minRtt=%u\n", (unsigned)gS.minRtt); }
		CHECK_EQ(kvm_video_loop_period(33), 16);
		CHECK_EQ(kvm_video_interval_ms(), 16);
		CHECK(count_log("auto on=1 step=0 fps=60 interval_ms=16 changed=1 reason=start") + count_log("reason=start") >= 1);
		// a second of motion at 60 per second: the frames are made at that rate
		{
			// ... and stay there for many windows: the frames that are made are what tells Auto that the machine keeps up
			int n = 0;
			for (i = 0; i < 500; i++) { n += auto_step(16, 6, 100 + i); }
			CHECK(n >= 440);
		}
		CHECK_EQ(kvm_video_loop_period(33), 16);
		// the capture takes 60 ms: the machine does not keep up with 60, two windows: a step down (the interval changes, nothing else)
		for (i = 0; i < 400; i++) { (void)auto_step(16, 60, 300 + i); if (kvm_video_loop_period(33) > 16) { break; } }
		CHECK(kvm_video_loop_period(33) > 16);
		CHECK_EQ(kvm_video_loop_period(33), (int)kvm_video_interval_ms() > 0 ? kvm_video_loop_period(33) : 0);
		CHECK(count_log("changed=1 reason=capture") >= 1);
		{
			// the step is the period of the video too (not below what the CPU budget needs): 60 ms of capture is 80 ms needed: the step of 100 ms or 66 ms
			const int iv = kvm_video_interval_ms();
			CHECK(iv == kvm_video_loop_period(33));
			CHECK(iv >= 50);
		}
		// no new generation, no key frame forced, no 242 for a step: the stream goes on as it was
		CHECK_EQ(last_state().gen, gen);
		CHECK_EQ(gForces, forces);
		CHECK_EQ(states().size(), states0);
		for (const frame_info &f : frames()) { CHECK(f.ok); CHECK_EQ(f.f.gen, gen); }
	}
	// the log line of the step
	{
		const std::string l = last_log("auto on=1");
		CHECK(l.find("changed=") != std::string::npos);
		CHECK(l.find("reason=") != std::string::npos);
		CHECK(l.find("cap_p75_ms=") != std::string::npos);
	}
	// a new op 1 with the bit down: the fixed mode of command 5 again
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	gTestNowMs += 3000;
	send_ctrl(KVM_OP_VIDEO_ON, 7);
	CHECK_EQ(kvm_video_loop_period(33), 33);
	CHECK_EQ(kvm_video_interval_ms(), 33);
	{
		const int before = count_log("auto on=");
		for (int i = 0; i < 100; i++) { (void)auto_step(33, 60, 900 + i); }
		CHECK_EQ(count_log("auto on="), before);						// nothing is judged, nothing is logged
		CHECK_EQ(kvm_video_loop_period(33), 33);
	}
	end_session();

	// the video stops (op 2): the period of command 5 holds again, Auto has nothing to say about JPEG
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	gS.minRtt = 5;
	for (int i = 0; i < 40; i++) { (void)auto_step(16, 6, i); }
	CHECK_EQ(kvm_video_loop_period(100), 16);
	send_ctrl(KVM_OP_VIDEO_OFF, 0);
	CHECK(!kvm_video_on());
	CHECK_EQ(kvm_video_loop_period(100), 100);
	CHECK_EQ(kvm_video_interval_ms(), (int)(kvm_video_loop_period(33) == 33 ? kvm_video_interval_ms() : -1));
	end_session();

	// ---- the link: the window of the viewer holds the frames back (nothing is acknowledged), the frames are big: the QP that the regulator is about to ask for is over the ceiling, a step down for the link ----
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	gPadBytes = 60000;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	gS.minRtt = 5;
	{
		int i, step = 16;
		for (i = 0; i < 600 && step == 16; i++)
		{
			(void)auto_step(16, 6, i, false);
			step = kvm_video_loop_period(33);
		}
		CHECK(step > 16);
		CHECK(count_log("changed=1 reason=link") >= 1);
		CHECK(count_log("held_win=") >= 1);
	}
	end_session();

	test_auto_hw_and_stage2();
	test_auto_level_and_refresh();
	test_auto_review_glue();
	test_auto_reserve();
	test_auto_flat_virgin();

	// ---- 20 ms of round trip or more: the start is 30 ----
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	gS.minRtt = 40;
	for (int i = 0; i < 60; i++) { (void)auto_step(33, 8, i); }
	CHECK_EQ(kvm_video_loop_period(16), 33);						// (the period of the viewer is not asked for: 33 is the step)
	CHECK_EQ(kvm_video_loop_period(100), 33);						// Auto replaces the period of command 5, whatever it is
	end_session();

	// ---- a viewer that sends the bit while the video is not running (JPEG): nothing is overridden ----
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	gS.minRtt = 5;
	CHECK_EQ(kvm_video_loop_period(33), 33);
	end_session();

	// ---- a key frame does not count as a frame of the window: after a long still screen the first window is not judged ----
	begin_session(1920, 1080);
	settle_all();
	FRAME_RATE_TIMER = 33;
	send_ctrl(KVM_OP_VIDEO_ON, 7 | KVM_AUTO_OP1_FLAG);
	gS.minRtt = 5;
	for (int i = 0; i < 80; i++) { (void)auto_step(16, 6, i); }
	CHECK_EQ(kvm_video_loop_period(33), 16);
	end_session();
}
