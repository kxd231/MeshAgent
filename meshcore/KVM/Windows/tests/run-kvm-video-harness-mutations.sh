#!/bin/sh
# Mutation check of kvm_video_harness_test.cpp: each line changes one rule of the glue (kvm_video.cpp) in a copy; the harness has to fail. A mutation that does not compile is INVALID,
# one that the harness does not notice SURVIVED (exit status 1 at the end).
# Usage: run-kvm-video-harness-mutations.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
KVM=$(cd "$HERE/../.." && pwd)
IMG=kvm-ctestpp:local
docker image inspect "$IMG" >/dev/null 2>&1 || { echo "run run-kvm-video-harness.sh first (it builds the image)"; exit 2; }
exec docker run --rm --label kvm-auto-agent=1 -e FROZEN="${FROZEN:-}" -e MUT_JOBS="${MUT_JOBS:-3}" -e MUT_FILTER="${MUT_FILTER:-}" --user "$(id -u):$(id -g)" -v "$KVM:/src:ro" -w /tmp "$IMG" sh -c '
set -u
cat > /tmp/mutations.txt <<"EOT"
kvm_video.cpp @@ if (n > 0) { tiles_to_jpeg(gV.map); gV.st.fallbackTiles += n; } @@ if (n > 0) { gV.st.fallbackTiles += n; } @@ video off leaves the pending tiles unsent (I7)
kvm_video.cpp @@ for (c = col0; c <= col1; c++) { if (tileInfo[r][c].refresh) { return 0; } } @@ for (c = col0; c <= col1; c++) { ; } @@ tiles of a REFRESH go to the video
kvm_video.cpp @@ t->sentQ = 1; t->sentSub = 1; @@ t->sentQ = 50; t->sentSub = 0; @@ painted tiles are not marked as blurred (never sharpened)
kvm_video.cpp @@ t->lastChange = now; @@ ; @@ painted tiles may be sharpened in the middle of the motion
kvm_video.cpp @@ if (t->crc != 0xFF) { t->crc = t->contentCrc; } @@ t->crc = t->contentCrc; @@ a REFRESH during the frame is forgotten
kvm_video.cpp @@ in.gateOpen = kvm_ack_peek(KVM_GATE_VIDEO); @@ in.gateOpen = 1; @@ a frame goes into a closed window
kvm_video.cpp @@ if (plan.held) { if (fromEmit) { if (kvm_ack_note_link_held()) { kvm_vrc_window_refused(&gV.rc); } else { kvm_vrc_window_refused_soft(&gV.rc); } gV.st.held++; } return 0; } // pendingVideo @@ if (plan.held) { return 0; } // pendingVideo @@ a refused frame is not noted as held back
kvm_video.cpp @@ if (plan.held) { if (fromEmit) { if (kvm_ack_note_link_held()) { kvm_vrc_window_refused(&gV.rc); } else { kvm_vrc_window_refused_soft(&gV.rc); } gV.st.held++; } return 0; } // pendingVideo @@ if (plan.held) { if (fromEmit) { kvm_ack_note_held_back(); kvm_vrc_window_refused(&gV.rc); gV.st.held++; } return 0; } // pendingVideo @@ a refused frame feeds the rate samples but is not link limited
kvm_video.cpp @@ if (fd == KVM_VF_BROKEN) @@ if (0) @@ a first frame that is not an IDR is sent
kvm_video.cpp @@ job.forceIdr = plan.forceIdr ? 1 : 0; @@ job.forceIdr = 0; @@ a key frame is never forced
kvm_video.cpp @@ if (kvm_vctl_frame_broken(&gV.ctl, oversize) == KVM_VB_ERROR) @@ if (0) @@ lost access units are never an error
kvm_video.cpp @@ seq = kvm_stream_add_position(s, now, total); @@ seq = 1; @@ a frame is not a position of the ring
kvm_video.cpp @@ if (r->code == MFH264_E_EXCEPTION) { video_error("encode", &err, 1); return; } @@ if (r->code == MFH264_E_EXCEPTION) { video_error("encode", &err, 0); return; } @@ an exception is an ordinary error (the encoder is closed afterwards)
kvm_video.cpp @@ if (gV.ctl.state == KVM_VS_ON && !v.videoOn) { stop_video("stream layer"); } @@ if (0) { stop_video("stream layer"); } @@ the video is not stopped when the stream layer switches it off
kvm_video.cpp @@ if (gV.ctl.inError) { kvm_vctl_limits(&gV.ctl, &c); send_current_state(KVM_REASON_ERROR); return; } @@ ; @@ op 1 in the hold-off after an error starts the encoder again
kvm_video.cpp @@ if (!kvm_vctl_open_allowed(&gV.ctl, now)) { gV.ctl.state = KVM_VS_WAIT; return; } @@ ; @@ the encoder is created more than once in 2 s
kvm_video.cpp @@ gV.profile = info.sps_profile_idc != 0 ? info.sps_profile_idc : gV.profile; @@ ; @@ the 242 does not carry the profile of the SPS
kvm_video.cpp @@ if (n > 0) { tiles_to_jpeg(gV.map); }\n		kvm_vctl_repair_now @@ kvm_vctl_repair_now @@ op 1 does not send the VIDEO tiles as JPEG again
kvm_video.cpp @@ if (kvm_vctl_op1(&gV.ctl, &c, gV.reqAt[i]) == KVM_VOP1_COALESCE) @@ if (0) @@ op 1 within 2 s is not joined
kvm_video.cpp @@ gV.startPending = 1; gV.startKind = START_GEOMETRY; @@ gV.startPending = 0; @@ a new size does not start a new generation
kvm_video.cpp @@ if (!check_open_sps(cw, ch)) { return; } @@ ; @@ the SPS of the encoder is not checked at open
kvm_video.cpp @@ if (reason != 0)\n		{\n			kvm_stats_log("video sps refused (IDR) @@ if (0)\n		{\n			kvm_stats_log("video sps refused (IDR) @@ an IDR with an SPS outside the limits is sent
kvm_video.cpp @@ if (fresh) { gV.ts = 0; gV.encAvgUs = 0; gV.avgAu = 0; } @@ gV.ts = 0; @@ the time stamps go back on a new generation of the same encoder
kvm_video.cpp @@ if (gV.ctl.inError) { kvm_vctl_limits(&gV.ctl, &c); @@ if (gV.ctl.inError) { @@ the limits of an op 1 in the hold-off are forgotten
kvm_video.cpp @@ kvm_vctl_pace_ok(&gV.ctl, timeGetTime(), frame_interval_ms()) && @@ 1 && @@ a picture is ordered before a frame may go
kvm_video.cpp @@ return kvm_pace_interval(&gV.budget, frame_period_ms(), gV.encAvgUs); @@ return frame_period_ms(); @@ the frame interval ignores the CPU budget of the encoder
kvm_video.cpp @@ if (fd == 0) { gV.ctl.lastFrame = sl->slotAt; } @@ ; @@ the slot of a frame is the moment it was written
kvm_video.cpp @@ ms = kvm_pace_wake_ms(1, gV.ctl.lastFrame, now, frame_interval_ms()); @@ ms = kvm_pace_wake_ms(1, gV.ctl.lastFrame, now, frame_interval_ms() + 1); @@ the loop sleeps past the slot of the frame
kvm_video.cpp @@ if (!kvm_ack_peek(KVM_GATE_VIDEO) || !kvm_vrc_peek(&gV.rc, now)) { return -1; } @@ ; @@ the loop sleeps for a slot although the window or the bucket holds the frame
kvm_video.cpp @@ gV.tiles.nPending > 0 && gV.ctl.haveFrame && kvm_encthr_free_slot() >= 0)) { return -1; } @@ gV.ctl.haveFrame && kvm_encthr_free_slot() >= 0)) { return -1; } @@ the loop sleeps for a slot with nothing pending
kvm_video.cpp @@ kvm_budget_note_cpu(&gV.budget, d > o ? d - o : 0ull); @@ kvm_budget_note_cpu(&gV.budget, d); @@ the CPU of the loop and of the capture thread is counted for the encoder
kvm_video.cpp @@ kvm_budget_note_encode(&gV.budget, r->encUs, 0); @@ ; @@ the parallelism of the encoder is never measured (no wall time of the frames)
kvm_video.cpp @@ uint32_t p = FRAME_RATE_TIMER >= 1 ? (uint32_t)FRAME_RATE_TIMER : shortest; @@ uint32_t p = 50; @@ the video inherits 50 ms
kvm_video.cpp @@ if (p < shortest) { p = shortest; } @@ ; @@ a period below 16 ms is taken
kvm_video.cpp @@ && 1000u / (uint32_t)gV.fpsCap > p) { p = 1000u / (uint32_t)gV.fpsCap; } @@ && 0) { p = 1; } @@ the level of the stream does not limit the frame rate
kvm_video.cpp @@ int kvm_video_active(void) { return (gV.sessionOn && gV.ctl.state != KVM_VS_OFF) ? 1 : 0; } @@ int kvm_video_active(void) { return kvm_video_on(); } @@ the video that waits for its encoder is not "active"
kvm_video.cpp @@ in.rateOpen = kvm_vrc_peek(&gV.rc, now); @@ in.rateOpen = 1; @@ the byte bucket of the rate control does not hold frames back
kvm_video.cpp @@ if (fromEmit) { (void)kvm_vrc_may_encode(&gV.rc, now, 0); gV.st.heldRate++; } @@ if (fromEmit) { gV.st.heldRate++; } @@ a refusal by the bucket is not counted for the regulator
kvm_video.cpp @@ kvm_vrc_frame(&gV.rc, now, (uint32_t)total, sl->qpUsed != 0 ? sl->qpUsed : gV.appliedQp, isIdr, gV.encAvgUs); @@ ; @@ the bytes of a frame are not debited from the bucket
kvm_video.cpp @@ (void)kvm_vrc_tick(&gV.rc, now); @@ ; @@ the QP regulator never runs
kvm_video.cpp @@ const unsigned budget = (unsigned)kvm_adapt_viewer_budget_kbps(); @@ const unsigned budget = 0; @@ the viewer budget is ignored by the video
kvm_video.cpp @@ int wantQp = kvm_vrc_qp(&gV.rc, isKey); @@ int wantQp = kvm_vrc_qp(&gV.rc, 0); @@ the IDR gets the QP of the P frames
kvm_video.cpp @@ if (!v.adaptOk && gV.tiles.st != NULL && gV.map != NULL && gV.tiles.nVideo > 0) @@ if (0) @@ VIDEO tiles stay blurred when the sharpening path disappears after the video was off
kvm_video.cpp @@ kvm_vctl_apply_limits(&gV.ctl); // the limits @@ // the limits @@ the limits of a joined op 1 are never applied
kvm_video.cpp @@ kvm_vrc_set_picture(&gV.rc, (uint32_t)cw * (uint32_t)ch); @@ ; @@ the expected size of the first key frame ignores the picture size
kvm_video.cpp @@ kvm_vrc_update_starved(&gV.rc, now) @@ 0 @@ the log never says starved
kvm_video.cpp @@ kvm_stream_state_msg(s, gV.ctl.capVideo ? KVM_REASON_ERROR : KVM_REASON_DISABLED, &m); @@ kvm_stream_state_msg(s, KVM_REASON_ERROR, &m); @@ the 3rd error says reason 4 (a retry is promised)
kvm_video.cpp @@ if (kvm_ack_note_link_held()) { kvm_vrc_window_refused(&gV.rc); } else { kvm_vrc_window_refused_soft(&gV.rc); } @@ if (kvm_ack_note_link_held()) { } else { kvm_vrc_window_refused_soft(&gV.rc); } @@ the rate control is not told that the window refused a frame
kvm_video.cpp @@ idrWhy = kvm_vctl_idr_why(&gV.ctl, &plan); @@ idrWhy = KVM_IDR_WHY_NONE; @@ the log does not know why a key frame came
kvm_video.cpp @@ kvm_idr_why_name(sl->idrWhy, isIdr), (unsigned)gV.ctl.frames, @@ "x", (unsigned)gV.ctl.frames, @@ the reason is not in the log line
kvm_video.cpp @@ if (actions & KVM_ACT_IDR) { gV.op3Prev = gV.op3At; gV.op3At = (DWORD)(atMs ? atMs : 1u); gV.op3Count++; gV.op3Flags = c->flags; } @@ ; @@ the requests of op 3 are not remembered for the log
kvm_video.cpp @@ kvm_adapt_note_video_frame(); // the sharpening governor: the video moves @@ ; @@ the sharpening governor is not told that a frame went out
kvm_video.cpp @@ wantQp += kvm_qpdet_probe_offset(&gV.qpdet, wantQp, KVM_VIDEO_QP_MIN, KVM_VIDEO_QP_MAX); @@ ; @@ step 5: the detector gets no deliberate QP step
kvm_video.cpp @@ if (gV.qpStage <= 1 && !isIdr) @@ if (0) @@ step 5: the detector sees no frames
kvm_video.cpp @@ == KVM_QPDET_INEFFECTIVE) { gV.qpRestart = gV.qpStage + 1; } @@ == KVM_QPDET_INEFFECTIVE) { ; } @@ step 5: nothing is done when the encoder does not listen
kvm_video.cpp @@ if (sqRc == 0) { gV.qpStage = 1; @@ if (0) { gV.qpStage = 1; @@ step 5: the QP is not put into the input sample
kvm_video.cpp @@ gV.qpStage = 2; gV.bitrateTry = 0; gV.appliedBps = 0; @@ gV.qpStage = 3; gV.bitrateTry = 0; gV.appliedBps = 0; @@ step 5: the bit rate mode is never entered
kvm_video.cpp @@ job.setBitrate = want; @@ job.setBitrate = 0; @@ step 5: the bit rate is not passed to the encoder
kvm_video.cpp @@ (want > gV.appliedBps + gV.appliedBps / 8 || want < gV.appliedBps - gV.appliedBps / 8) @@ (want > gV.appliedBps + gV.appliedBps / 8) @@ step 5: the bit rate does not follow R downwards
kvm_video.cpp @@ gV.bitrateTry++; close_encoder(); gV.startPending = 1; @@ close_encoder(); gV.startPending = 1; @@ step 5: a refused rate control mode is tried again
kvm_video.cpp @@ cfg.rc = o->bitrateTry == 0 ? MFH264_RC_CBR : (o->bitrateTry == 1 ? MFH264_RC_LDVBR : MFH264_RC_PEAKVBR); @@ cfg.rc = MFH264_RC_CBR; @@ step 5: only CBR is tried
kvm_video.cpp @@ pending_to_jpeg();\n				close_encoder();\n				gV.ctl.state = KVM_VS_WAIT; @@ close_encoder();\n				gV.ctl.state = KVM_VS_WAIT; @@ step 5: the tiles that wait for a frame are lost at the change of the encoder
kvm_video.cpp @@ kvm_vrc_note_sharpen(&gV.rc, timeGetTime(), bytes, videoActive); @@ ; @@ the rate control is not told about the sharpening
kvm_video.cpp @@ if (r->code == MFH264_E_INPUT && gV.qpStage == 1) @@ if (0) @@ a refused QP attribute is an error of the encoder
kvm_video.cpp @@ \t\tgV.qpRestart = 2;\n\t\treturn;\n\t} @@ \t\tgV.qpRestart = 0;\n\t\treturn;\n\t} @@ a refused QP attribute does not lead to the bit rate mode
kvm_video.cpp @@ if (isIdr) { kvm_stream_watch(s, seq); gV.idrWatchBytes = (uint32_t)total; } @@ ; @@ the position of the key frame is not watched
kvm_video.cpp @@ kvm_vrc_idr_acked(&gV.rc, now, gV.idrWatchBytes, ts, ackAt, queueMs, minRtt); @@ ; @@ what the link took of the key frame is not given to the rate control
kvm_video.cpp @@ if (gV.ctl.state == KVM_VS_ON && gV.idrWatchBytes != 0) @@ if (0) @@ the acknowledgement of the key frame is never looked at
kvm_video.cpp @@ if (w > ms) { ms = w; } @@ ; @@ the loop does not wake for the write gap (it wakes for the slot only)
kvm_video.cpp @@ kvm_vctl_note_write(&gV.ctl, now, (gV.encAvgUs + 500u) / 1000u); @@ ; @@ the write of a frame is not told to the pace (the frame after a late one comes right behind it)
kvm_video.cpp @@ if (job->gen != curGen) @@ if (0) @@ the encoder thread: a result of an old generation is written
kvm_video.cpp @@ 	if (kvm_vtiles_rect_inflight(&gV.tiles, row0, col0, row1, col1)) { barrier(&gV.st.barJpeg); } @@ 	; @@ the encoder thread: a JPEG of a tile in flight does not wait for the frame
kvm_video.cpp @@ 			barrier(&gV.st.barRefresh);		// a frame in flight @@ 			// a frame in flight @@ the encoder thread: a REFRESH does not wait for the frame in flight
kvm_video.cpp @@ 		barrier(&gV.st.barRepair);\n @@ 		\n @@ the encoder thread: a repair does not wait for the frame in flight
kvm_video.cpp @@ if (kvm_vtiles_get(&gV.tiles, r, c) == KVM_VT_PENDING || t->contentCrc != sl->paintCrc[i]) { continue; } @@ if (0) { continue; } @@ a tile that changed while its frame was in flight is taken for painted (its change is lost)
kvm_video.cpp @@ if (kvm_vtiles_get(&gV.tiles, r, c) == KVM_VT_PENDING || t->contentCrc != sl->paintCrc[i]) { continue; } @@ if (kvm_vtiles_get(&gV.tiles, r, c) == KVM_VT_PENDING) { continue; } @@ a change that was held back (deferred) is cleared by the frame that did not show it
kvm_video.cpp @@ 		(void)kvm_vtiles_abort(&gV.tiles, slot);\n		gV.st.broken++;\n		kvm_stats_log("video lost access unit @@ 		gV.st.broken++;\n		kvm_stats_log("video lost access unit @@ the encoder thread: the tiles of a lost access unit are not painted again
kvm_video.cpp @@ if (slotIdx < 0) { gV.heldBusy = 1; if (fromEmit) { gV.st.busyHeld++; } return 0; } @@ if (slotIdx < 0) { gV.held = NULL; return 0; } @@ the picture is dropped when the encoder is busy
kvm_video.cpp @@ if (gV.held != NULL && gV.heldBusy && gV.sessionOn && try_start(0) && gV.encMode == KVM_ENCTHR_INLINE) @@ if (0) @@ the held picture is not started when the encoder is done
kvm_video.cpp @@ kvm_stream_set_reserved(kvm_glue_stream(), sl->estBytes); @@ kvm_stream_set_reserved(kvm_glue_stream(), 0); @@ the frame in the encoder is not in the window of the JPEG
kvm_video.cpp @@ 	if (kvm_encthr_oldest_age(timeGetTime()) > KVM_ENC_WATCHDOG_MS) { encoder_hung("watchdog"); } @@ 	; @@ the encoder thread: a hung encoder is never given up
kvm_video.cpp @@ (void)kvm_encthr_abandon();\n @@ \n @@ the encoder thread: the hung encoder is not abandoned (its buffers are freed under it)
kvm_video.cpp @@ 	kvm_vtiles_free(&gV.tiles);\n	free_slot_maps();\n	free(gV.map); @@ 	free_slot_maps();\n	free(gV.map); @@ a new geometry does not free the old tile model (a leak)
kvm_video.cpp @@ (void)kvm_vtiles_land(&gV.tiles, slot, gV.map);\n	tiles_painted(gV.map, sl, now); @@ tiles_painted(gV.map, sl, now); @@ the tiles of a written frame never land (they stay in flight)
kvm_video.cpp @@ if (gV.held != NULL && gV.heldBusy && gV.sessionOn @@ if (gV.held != NULL && gV.sessionOn @@ the service starts every held picture (the delay is latched: the frames start one iteration behind)
kvm_video.cpp @@ gV.heldBusy = 1; if (fromEmit) { gV.st.busyHeld++; } @@ if (fromEmit) { gV.st.busyHeld++; } @@ a picture held for a busy encoder is never started at the landing
kvm_video.cpp @@ 	while (collect_one()) { }				// a frame that the encoder finished during the tile loop @@ 				// a frame that the encoder finished during the tile loop @@ emit does not collect a finished frame
kvm_video.cpp @@ 	if (isKey && gV.rc.lastIdrBytes > sl->estBytes) { sl->estBytes = gV.rc.lastIdrBytes; } @@ 	; @@ a key frame reserves the average of the P frames in the window
kvm_video.cpp @@ 		gV.appliedQp = 0; gV.appliedBps = 0; @@ 		; @@ after a job that was given up the QP and the bit rate are taken for set
kvm_video.cpp @@ kvm_vctl_set_threaded(&gV.ctl, gV.encMode != KVM_ENCTHR_INLINE); @@ kvm_vctl_set_threaded(&gV.ctl, 0); @@ the write gap caps the encode time of the threaded encoder at half the interval
kvm_video.cpp @@ gV.encMode == KVM_ENCTHR_INLINE ? gV.inlineCpuUs : 0ull @@ 0ull @@ the inline mode does not measure the CPU of the encoder
kvm_video.cpp @@ 	if (kvm_fbuf_refs(desktop) < 0) { pending_to_jpeg(); return; } @@ 	; @@ a picture that is not a buffer of kvm_fbuf is held
kvm_video.cpp @@ job.setQp = gV.hw.active ? kvm_vcore_hw_qp(wantQp) : wantQp; @@ job.setQp = wantQp; @@ the hardware encoder is given the QP of the regulator (no offset, no floor)
kvm_video.cpp @@ gV.appliedQp = wantQp; gV.qp = wantQp; @@ gV.appliedQp = job.setQp; gV.qp = job.setQp; @@ the rate control remembers the QP that the encoder was given, not the one of the regulator
kvm_video.cpp @@ if (exc) { hw_die(what); } @@ ; @@ an exception in the driver does not end the process
kvm_video.cpp @@ gV.hw.bypassOpen = 1; gV.hw.rebuild = 1; @@ gV.hw.rebuild = 1; @@ the return to the software encoder waits for the limit of one open in 2 s
kvm_video.cpp @@ gV.hw.tryOpen = 1; @@ ; @@ the marker is written but the code does not know it has to clear it
kvm_video.cpp @@ if (gV.encMode == KVM_ENCTHR_INLINE) { snprintf(gV.hw.why, sizeof(gV.hw.why), "inline_mode"); return 0; } @@ if (0) { snprintf(gV.hw.why, sizeof(gV.hw.why), "inline_mode"); return 0; } @@ the hardware encoder is used in the inline mode of the encoder thread
kvm_video.cpp @@ gV.hw.mode = kvm_hwmode_read(); @@ gV.hw.mode = KVM_HWP_MODE_AUTO; @@ the mode file is not read
kvm_video.cpp @@ in.displayActive = (kvm_capture_state() == 2) ? 1 : 0; @@ in.displayActive = 1; @@ the hardware encoder is started in the lid-closed state
kvm_video.cpp @@ in.desktopDefault = desktop_is_default(); @@ in.desktopDefault = 1; @@ the self test is started on the Winlogon desktop
kvm_video.cpp @@ strcmp(res.key, gV.hw.key) != 0) @@ 0) @@ the result of a self test of another key is taken
kvm_video.cpp @@ if (verdict == KVM_HWP_V_PASS && !gV.hw.active && !gV.hw.disabled) { gV.hw.switchPending = 1; @@ if (!gV.hw.active && !gV.hw.disabled) { gV.hw.switchPending = 1; @@ the switch is armed by every verdict
kvm_video.cpp @@ kvm_vrc_init_backend(&gV.rc, gV.rc.qp, now, gV.hw.active); @@ kvm_vrc_init(&gV.rc, gV.rc.qp, now); @@ the new regulator forgets that the encoder is the hardware one
kvm_video.cpp @@ gV.hw.disabled = 1;\n	if (weight <= 0) @@ if (weight <= 0) @@ the hardware encoder is not off for the rest of the process
kvm_video.cpp @@ o.op = CO_ERROR; o.key = gV.hw.key; o.weight = weight; @@ o.op = CO_ERROR; o.key = gV.hw.key; o.weight = 1; @@ a hang is an error of weight 1
kvm_video.cpp @@ gV.level = (info.sps_level_idc != 0 && !gV.hw.active) ? info.sps_level_idc : level; @@ gV.level = info.sps_level_idc != 0 ? info.sps_level_idc : level; @@ the level that is announced is the one of the sequence header of the hardware encoder
kvm_video.cpp @@ if (!gV.hw.active && !check_open_sps(cw, ch)) { return; } @@ if (!check_open_sps(cw, ch)) { return; } @@ the sequence header of the hardware encoder is judged
kvm_video.cpp @@ gV.qpStage = 0;						// the hardware encoder has the QP @@ ;						// the hardware encoder has the QP @@ the hardware encoder keeps a stage of the software encoder
kvm_video.cpp @@ hw_poll_test();\n @@ ;\n @@ the result of the self test is never collected
kvm_video.cpp @@ kvm_hwtest_abort(&gV.hw.run); @@ ; @@ the self test is not stopped with the session
kvm_video.cpp @@ hw_fallback("hung", 2); @@ video_error("encoder hung", NULL, 1); @@ a hang of the hardware encoder given up by the watchdog ends the video instead of the fallback
kvm_video.cpp @@ hw_fallback("hung", 2); @@ hw_fallback("hung", 1); @@ a hang in the driver is an error of weight 1
kvm_video.cpp @@ gV.haveEnc = 0;\n		hw_fallback("hung", 2); @@ hw_fallback("hung", 2); @@ the handle of the given-up thread is closed by the fallback
kvm_video.cpp @@ else if (gV.hw.tryOpen) { hw_end_try(); } @@ ; @@ the software encoder takes over in an orderly way and the marker of the hardware stays
kvm_video.cpp @@ memset(&uo, 0, sizeof(uo)); uo.op = CO_USED; uo.key = gV.hw.key;\n\t\tcache_do(&uo); @@ memset(&uo, 0, sizeof(uo));\n\t\t @@ the marker is never in use
kvm_video.cpp @@ gV.hw.tryUsed = 1;\n\t\tmemset @@ memset @@ the first access unit is written into the cache at every frame
kvm_video.cpp @@ if (out.useHardware && !gV.hw.tryOpen && !dry) @@ if (out.useHardware && !dry) @@ the marker is written at every open (twice per generation)
kvm_video.cpp @@ if (cache_do(&mo) != 0) @@ if (cache_do(&mo) != 0 && 0) @@ a marker that cannot be written does not stop the hardware encoder
kvm_video.cpp @@ gV.hw.disabled = 1;\n			snprintf(gV.hw.why, sizeof(gV.hw.why), "marker_write_failed"); @@ snprintf(gV.hw.why, sizeof(gV.hw.why), "marker_write_failed"); @@ the hardware encoder is tried again after a marker that could not be written
kvm_video.cpp @@ in.hwRunning = wasHw; @@ in.hwRunning = 0; @@ the hardware encoder that runs is not kept when the lid is closed
kvm_video.cpp @@ if (!in.displayActive && !in.hwRunning) { in.hwPresent = 1; @@ if (0) { in.hwPresent = 1; @@ the encoders are enumerated although the capture is GDI
kvm_video.cpp @@ if (!gV.hw.selDone || gV.hw.selLuid != luid) @@ if (1) @@ the enumeration is asked at every generation
kvm_video.cpp @@ gV.hw.disabled = 1;\n\t\t\t\tif (gV.hw.selKeyOk) @@ if (gV.hw.selKeyOk) @@ after a hang of the enumeration the hardware is not off in this process
kvm_video.cpp @@ if (gV.hw.selKeyOk) { memset(&so, 0, sizeof(so)); @@ if (0) { memset(&so, 0, sizeof(so)); @@ a hang of the enumeration is not written down
kvm_video.cpp @@ kvm_hwp_is_bad(se, in.nowSec) && in.env != KVM_HWP_ENV_FORCE && in.mode != KVM_HWP_MODE_HW @@ 0 @@ the entry of a hang of the enumeration is not looked at
kvm_video.cpp @@ kvm_hwp_is_bad(se, in.nowSec) && in.env != KVM_HWP_ENV_FORCE && in.mode != KVM_HWP_MODE_HW @@ kvm_hwp_is_bad(se, in.nowSec) && in.env != KVM_HWP_ENV_FORCE @@ the mode file hw does not try again after a hang of the enumeration
kvm_video.cpp @@ hw_fallback("sps_refused", 0); @@ hw_fallback("sps_refused", 1); @@ an SPS that the viewer refuses is an error of the hardware
kvm_video.cpp @@ if (gV.hw.active)\n\t\t\t{\n\t\t\t\t// the SPS of the hardware @@ if (0)\n\t\t\t{\n\t\t\t\t// the SPS of the hardware @@ an SPS of the hardware encoder that the viewer refuses ends the video
kvm_video.cpp @@ const int weight = exc ? 2 : 1; @@ const int weight = (exc || (err != NULL && err->code == MFH264_E_HANG)) ? 2 : 1; @@ one deadline that passed makes the entry bad for two weeks
kvm_video.cpp @@ kvm_hwmode_read() == KVM_HWP_MODE_SW) { gV.hw.switchPending = 0; } @@ 0) { gV.hw.switchPending = 0; } @@ the switch after the self test ignores the mode file
kvm_video.cpp @@ if (weight <= 0) { hw_end_try(); } @@ if (0) { hw_end_try(); } @@ a fallback without an error leaves the marker
kvm_video.cpp @@ if (kvm_capture_state() != 0) { gV.hw.capWaitAt = 0; return 0; } @@ if (1) { gV.hw.capWaitAt = 0; return 0; } @@ the start never waits for the capture
kvm_video.cpp @@ (DWORD)(now - gV.hw.capWaitAt) >= KVM_HW_CAP_WAIT_MS @@ 0 @@ the wait for the capture never ends
kvm_video.cpp @@ gV.hw.capGaveUp = 1; gV.hw.capWaitAt = 0; @@ gV.hw.capWaitAt = 0; @@ every generation waits for the capture again
kvm_video.cpp @@ kvm_hwenv_read() == KVM_HWP_ENV_OFF || kvm_hwmode_read() == KVM_HWP_MODE_SW || kvm_hwp_size_class @@ 0 || kvm_hwmode_read() == KVM_HWP_MODE_SW || kvm_hwp_size_class @@ the start waits for the capture although the switch is off
kvm_video.cpp @@ kvm_hwenv_read() == KVM_HWP_ENV_OFF || kvm_hwmode_read() == KVM_HWP_MODE_SW || kvm_hwp_size_class @@ kvm_hwenv_read() == KVM_HWP_ENV_OFF || 0 || kvm_hwp_size_class @@ the start waits for the capture although the mode file says sw
kvm_video.cpp @@ in.displayActive = (kvm_capture_state() == 2) ? 1 : 0; @@ in.displayActive = (kvm_capture_state() != 1) ? 1 : 0; @@ a capture that is not known is taken for DXGI
kvm_video.cpp @@ hw_recheck(now); @@ ; @@ a capture that becomes DXGI does not ask for another decision
kvm_video.cpp @@ if (cap != 2 || was == 2 || @@ if (cap != 2 || @@ another decision at every look while the capture is DXGI
kvm_video.cpp @@ if (quiet || due) @@ if (quiet) @@ a screen that never rests never gets the hardware encoder
kvm_video.cpp @@ gV.hw.switchPending = 1; gV.hw.switchSince = timeGetTime(); @@ gV.hw.switchPending = 1; @@ the switch after the self test has no time limit
kvm_video.cpp @@ if (out.useHardware && !gV.hw.tryOpen && !dry) @@ if (out.useHardware && !gV.hw.tryOpen) @@ a decision that is not a start writes the marker
kvm_video.cpp @@ if (gV.view.ok && kvm_nv12v_plan( @@ if (0 && gV.view.ok && kvm_nv12v_plan( @@ the encoder is never given the view
kvm_video.cpp @@ job.viewFull = vp.full; @@ job.viewFull = 0; @@ a key frame is not complete
kvm_video.cpp @@ job.viewMap = vp.map; @@ ; @@ the plan of the rectangles does not reach the job
kvm_video.cpp @@ gV.view.on = env_flag("MESH_KVM_NV12_FROZEN", KVM_NV12_FROZEN_DEFAULT); @@ gV.view.on = 0; @@ the switch of the view is never on
kvm_video.cpp @@ #define KVM_NV12_FROZEN_DEFAULT 1 @@ #define KVM_NV12_FROZEN_DEFAULT 0 @@ the default of the view in the product is off (the harness is built without -DKVM_NV12_FROZEN_DEFAULT)
kvm_video.cpp @@ gV.view.flat = gV.view.on && env_flag("MESH_KVM_IDR_FLAT", KVM_IDR_FLAT_DEFAULT); @@ gV.view.flat = 0; @@ the flat key frame is never on
kvm_video.cpp @@ gV.view.firstFull = env_flag("MESH_KVM_IDR_FLAT_FIRST_FULL", KVM_IDR_FLAT_FIRST_FULL_DEFAULT); @@ gV.view.firstFull = 0; @@ the first key frame of the session is flat although it is to be complete
kvm_video.cpp @@ fresh = 1;\n		view_reset(cw, ch); @@ fresh = 1; @@ a new encoder keeps the view of the old one (another size: a plan for another grid)
kvm_video.cpp @@ kvm_nv12v_free(&gV.view.v); gV.view.ok = 0;\n @@ \n @@ the NV12 view leak: the view of the loop is not freed at the end of the session
kvm_video.cpp @@ job.viewMode = 1; job.viewFull = vp.full; @@ job.viewMode = 0; job.viewFull = vp.full; @@ a job is encoded as the picture
kvm_video.cpp @@ if (sl->virginMbs == 0 && kvm_qpdet_frame( @@ if (kvm_qpdet_frame( @@ a frame of virgin macroblocks is an event of the QP detector
kvm_video.cpp @@ if (gV.view.flat) { kvm_vrc_set_idr_cover(&gV.rc, (isKey && !vp.full) ? vp.realMbs : 0u, vp.totalMbs); } @@ if (gV.view.flat) { kvm_vrc_set_idr_cover(&gV.rc, 0u, 0u); } @@ the rate control is not told the area of the key frame
kvm_video.cpp @@ if (gV.view.flat && gV.qpStage != 2)\n	{ @@ if (0 && gV.view.flat && gV.qpStage != 2)\n	{ @@ the window is reserved for the whole picture
kvm_video.cpp @@ else { sl->estBytes = gV.avgAu + kvm_vrc_virgin_bytes( @@ else { sl->estBytes = gV.avgAu + 0 * kvm_vrc_virgin_bytes( @@ the virgin macroblocks of a P frame are not reserved
kvm_video.cpp @@ isIdr, gV.encAvgUs, virginBytes); @@ isIdr, gV.encAvgUs, 0u); @@ the regulator counts the virgin bytes as content
kvm_video.cpp @@ const unsigned virginBytes = (gV.view.flat && !isIdr && sl->virginMbs > 0) ? @@ const unsigned virginBytes = (0 && gV.view.flat && !isIdr && sl->virginMbs > 0) ? @@ no bytes are taken for virgin macroblocks
kvm_video.cpp @@ o.idrKb = res.idrKb; o.flatMilli = res.flatMilli; @@ o.idrKb = res.idrKb; @@ the floor of a flat key frame of the self test is not kept in the cache
kvm_video.cpp @@ if (fresh && gV.hw.active && gV.hw.flatMilli != 0) { kvm_vrc_set_flat_floor(&gV.rc, gV.hw.flatMilli); } @@ ; @@ the rate control does not get the floor that the self test measured
../common/kvm_nv12view.c @@ for (r = 0; r < mbRows; r++)\n	{\n		const uint8_t *row = map @@ for (r = 0; r < mbRows - 1; r++)\n	{\n		const uint8_t *row = map @@ the last row of macroblocks of a frame is not converted (the model of random sessions reads the whole tile of the view)
../common/kvm_nv12view.c @@ c * KVM_NV12V_MB, r * KVM_NV12V_MB, (c1 - c) * KVM_NV12V_MB, @@ c * KVM_NV12V_MB, r * KVM_NV12V_MB, (c1 - c - (c1 == mbCols && c1 - c > 1 ? 1 : 0)) * KVM_NV12V_MB, @@ the last macroblock of a row is not converted (the model of random sessions reads the whole tile of the view)
kvm_video.cpp @@ kvm_auto_set(&gV.au, (gV.autoOk && gV.qpStage < 2 && (c.flags & KVM_AUTO_OP1_FLAG)) ? 1 : 0, now); @@ kvm_auto_set(&gV.au, 1, now); @@ every op 1 is Auto
kvm_video.cpp @@ kvm_auto_set(&gV.au, (gV.autoOk && gV.qpStage < 2 && (c.flags & KVM_AUTO_OP1_FLAG)) ? 1 : 0, now); @@ kvm_auto_set(&gV.au, 0, now); @@ bit 3 of the flags of op 1 does not turn Auto on
kvm_video.cpp @@ kvm_auto_set(&gV.au, (gV.autoOk && gV.qpStage < 2 && (c.flags & KVM_AUTO_OP1_FLAG)) ? 1 : 0, now); @@ kvm_auto_set(&gV.au, (gV.qpStage < 2 && (c.flags & KVM_AUTO_OP1_FLAG)) ? 1 : 0, now); @@ MESH_KVM_AUTO=0 does not switch the bit of op 1 off
kvm_video.cpp @@ return (gV.sessionOn && gV.qpStage < 2 && (gV.ctl.state == KVM_VS_ON || gV.ctl.state == KVM_VS_WAIT)) ? kvm_auto_interval_ms(&gV.au) : 0u; @@ return (gV.sessionOn && gV.qpStage < 2) ? kvm_auto_interval_ms(&gV.au) : 0u; @@ the step of Auto holds while the video is off
kvm_video.cpp @@ if (gV.autoOk && gV.qpStage < 2 && (m->caps & KVM_CAPS_H264)) { m->caps @@ if (gV.qpStage < 2 && (m->caps & KVM_CAPS_H264)) { m->caps @@ the 242 announces Auto although MESH_KVM_AUTO=0
kvm_video.cpp @@ if (gV.autoOk && gV.qpStage < 2 && (m->caps & KVM_CAPS_H264)) { m->caps @@ if (gV.autoOk && gV.qpStage < 2) { m->caps @@ the 242 announces Auto without video
kvm_video.cpp @@ { const uint32_t ai = auto_interval_ms(); if (ai > 0) { p = ai < shortest ? shortest : ai; } } @@ /* removed */ @@ the step of Auto is not the interval of the video
kvm_video.cpp @@ return ai > 0 ? (int)ai : viewerPeriodMs; @@ return viewerPeriodMs; @@ the pause of the loop ignores the step of Auto
kvm_video.cpp @@ kvm_auto_note_image(&gV.au, timeGetTime(), gV.tiles.nPending); @@ /* removed */ @@ the images are not noted
kvm_video.cpp @@ if (fromEmit && plan.held) { kvm_auto_note_held(&gV.au, 0); } @@ /* removed */ @@ a refusal of the window of the viewer is not noted
kvm_video.cpp @@ auto_tick(now, &v); @@ /* removed */ @@ the window of Auto is never closed
kvm_video.cpp @@ if (gV.sessionOn) { kvm_auto_note_capture(&gV.au, ms, dwmFrames); } @@ /* removed */ @@ the time of the capture is not noted
kvm_video.cpp @@ e.minRttMs = minRtt; @@ e.minRttMs = 0; @@ the round trip is not given to Auto (the start waits for 3 s)
kvm_video.cpp @@ gV.autoOk = !(GetEnvironmentVariableA( @@ gV.autoOk = 1 || !(GetEnvironmentVariableA( @@ MESH_KVM_AUTO=0 is not read
kvm_video.cpp @@ if (fromEmit && plan.heldRate) { kvm_auto_note_held(&gV.au, 1); } @@ /* removed */ @@ a refusal of the byte bucket is not noted
kvm_video.cpp @@ if (isIdr) { kvm_auto_note_key(&gV.au, now, @@ if (0) { kvm_auto_note_key(&gV.au, now, @@ a key frame does not freeze the windows
kvm_video.cpp @@ e.qpNow = (uint32_t)gV.rc.qp; @@ e.qpNow = 0u; @@ the QP of the regulator is not given to Auto
kvm_video.cpp @@ e.queueMs = fl > minRtt ? fl - minRtt : 0u; @@ e.queueMs = 0u; @@ the queue at the link is not given to Auto
kvm_video.cpp @@ e.queueMs = fl > minRtt ? fl - minRtt : 0u; @@ e.queueMs = fl; @@ the round trip is not taken off the data in flight
kvm_video.cpp @@ e.floorMs = gV.encAvgUs > 0 ? kvm_budget_enc_floor_ms(&gV.budget, gV.encAvgUs) : 0u; @@ e.floorMs = 0u; @@ the floor of the CPU budget is not given to Auto
kvm_video.cpp @@ else { kvm_auto_note_frame(&gV.au, now, sl->qpUsed != 0 ? sl->qpUsed : gV.appliedQp, (uint32_t)total); } @@ else { kvm_auto_note_frame(&gV.au, now, 0, (uint32_t)total); } @@ the QP of the frames is not noted
kvm_video.cpp @@ e.encAvgUs = gV.encAvgUs; @@ e.encAvgUs = 0; @@ the encode time is not given to Auto
kvm_video.cpp @@ if (gV.ctl.state == KVM_VS_OFF) { kvm_auto_resume(&gV.au, now); } @@ /* removed */ @@ a pause of the video leaves the window and the step
kvm_video.cpp @@ if (gV.fpsCap > 0 && (uint32_t)gV.fpsCap < KVM_VIDEO_MAX_FPS) { e.maxFps = (uint32_t)gV.fpsCap; e.maxReason = KVM_AUTO_R_SCREEN; } @@ /* removed */ @@ the level of the stream is not a ceiling of Auto
kvm_video.cpp @@ kvm_auto_note_key(&gV.au, now, gV.rc.rate > 0 ? (uint32_t)((unsigned long long)gV.codedW @@ if (0) kvm_auto_note_key(&gV.au, now, gV.rc.rate > 0 ? (uint32_t)((unsigned long long)gV.codedW @@ a REFRESH does not freeze the window
kvm_video.cpp @@ kvm_auto_set(&gV.au, 0, 0);			// the bit rate steers the encoder @@ 			// the bit rate steers the encoder @@ stage 2 does not end the Auto of the agent
kvm_video.cpp @@ if (gV.autoOk && gV.qpStage < 2 && (m->caps & KVM_CAPS_H264)) { @@ if (gV.autoOk && (m->caps & KVM_CAPS_H264)) { @@ stage 2 still announces Auto
kvm_video.cpp @@ e.qpFloor = (uint32_t)gV.rc.qpMin; @@ e.qpFloor = 0u; @@ the floor of the regulator is not given to Auto (equivalent in the glue: the floor of 30 never decides against the threshold of 31; the decision is tested in kvm_auto_test.c)
kvm_video.cpp @@ e.rateBps = (uint32_t)(gV.rc.rate > 0xFFFFFFFFull ? 0xFFFFFFFFull : gV.rc.rate); @@ e.rateBps = 0u; @@ R is not given to Auto
kvm_video.cpp @@ else { kvm_auto_note_frame(&gV.au, now, sl->qpUsed != 0 ? sl->qpUsed : gV.appliedQp, (uint32_t)total); } @@ /* removed */ @@ the frames that were made are not noted
kvm_video.cpp @@ if (!isIdr && virginBytes > 0) { kvm_auto_note_key( @@ if (0) { kvm_auto_note_key( @@ a P frame of virgin macroblocks (flat key frame) does not freeze the window
EOT
run_one() { # $1 = number, $2 = the line: the result goes to /tmp/res.$1
  idx=$1; line=$2
  file=$(printf "%s" "$line" | awk -F " @@ " "{print \$1}")
  search=$(printf "%s" "$line" | awk -F " @@ " "{print \$2}")
  replace=$(printf "%s" "$line" | awk -F " @@ " "{print \$3}")
  desc=$(printf "%s" "$line" | awk -F " @@ " "{print \$4}")
  d=/tmp/m$idx; rm -rf $d && mkdir -p $d && cp -r /src $d/kvm && cp $d/kvm/Windows/tests/shim/Windows.h $d/kvm/Windows/tests/shim/windows.h
  S="$search" R="$replace" perl -0pi -e "BEGIN { \$s = \$ENV{S}; \$r = \$ENV{R}; \$s =~ s/\\\\n/\\n/g; \$r =~ s/\\\\n/\\n/g; \$s =~ s/\\\\t/\\t/g; \$r =~ s/\\\\t/\\t/g; } \$c += s/\\Q\$s\\E/\$r/; END { exit(\$c == 1 ? 0 : 3) }" $d/kvm/Windows/$file
  if [ $? -ne 0 ]; then echo "INVALID  ($desc): the text was not found exactly once" > /tmp/res.$idx; return; fi
  cd $d/kvm/Windows/tests
  SAN=""; case "$desc" in *leak*) SAN="-fsanitize=address";; esac
  if ! ( gcc -std=c99 -O1 -w -c ../../common/kvm_video_core.c -o vc.o && gcc -std=c99 -O1 -w -c ../../common/kvm_stream.c -o vs.o && gcc -std=c99 -O1 -w -c ../../common/kvm_annexb.c -o va.o && gcc -std=c99 -O1 -w -c ../../common/kvm_video_rc.c -o vr.o && gcc -std=c99 -O1 -w -c ../../common/kvm_pace.c -o vp.o && gcc -std=c99 -O1 -w -c ../../common/kvm_qpdet.c -o vq.o && gcc -std=c99 -O1 -w -c ../../common/kvm_encjob.c -o ve.o && gcc -std=c99 -O1 -w -c ../../common/kvm_fbuf.c -o vf.o && gcc -std=c99 -O1 -w -c ../../common/kvm_hwpolicy.c -o vh.o && gcc -std=c99 -O1 -w -c ../../common/kvm_auto.c -o vau.o && gcc -std=c99 -O1 -w -c ../../common/kvm_nv12view.c -o vn.o && gcc -std=c99 -O1 -w -c ../../common/kvm_yuv.c -o vy.o && g++ -std=c++17 -O1 -w $SAN -fno-strict-aliasing -D_LINKVM -DKVM_DXGI -DKVM_DXGI_DISABLE -D_M_X64 -D_TURBOJPEG -DWIN32 -DWIN64 ${FROZEN:+-DKVM_NV12_FROZEN_DEFAULT=$FROZEN} -DKVM_VIDEO_TEST -Ishim -I.. -I../../common -pthread -o harness kvm_video_harness_test.cpp ../kvm_video.cpp ../kvm_encthr.cpp ../kvm_encbe_mf.cpp vc.o vs.o va.o vr.o vp.o vq.o ve.o vf.o vh.o vau.o vn.o vy.o ) 2>$d/err; then echo "INVALID  ($desc): does not compile" > /tmp/res.$idx; return; fi
  if HARNESS_SESSIONS=120 timeout 300 ./harness >$d/out 2>&1; then echo "SURVIVED ($desc)" > /tmp/res.$idx; else echo "killed   ($desc): $(grep -m1 FAIL $d/out | cut -c1-100)" > /tmp/res.$idx; fi
  cd /tmp; rm -rf $d
}
if [ -n "${MUT_FILTER:-}" ]; then grep -E "$MUT_FILTER" /tmp/mutations.txt > /tmp/mut_run.txt; else cp /tmp/mutations.txt /tmp/mut_run.txt; fi
n=0; running=0
while IFS= read -r line; do
  [ -z "$line" ] && continue
  n=$((n + 1))
  run_one $n "$line" &
  running=$((running + 1))
  if [ $running -ge ${MUT_JOBS:-3} ]; then wait; running=0; fi
done < /tmp/mut_run.txt
wait
survived=0; invalid=0; killed=0; i=1
while [ $i -le $n ]; do
  cat /tmp/res.$i
  case "$(cat /tmp/res.$i)" in SURVIVED*) survived=$((survived + 1));; INVALID*) invalid=$((invalid + 1));; *) killed=$((killed + 1));; esac
  i=$((i + 1))
done
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
