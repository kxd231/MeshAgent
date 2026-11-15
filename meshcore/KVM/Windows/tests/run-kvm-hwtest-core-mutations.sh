#!/bin/sh
# Mutation check of the tests of kvm_hwtest_core.c: each line changes one rule of the checks in a copy, and the tests have to turn red; INVALID = the text was not found or does not compile, SURVIVED = not noticed (exit 1).
# Usage: run-kvm-hwtest-core-mutations.sh
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
KVM=$(cd "$HERE/../.." && pwd)
IMG=kvm-ctest:local
exec docker run --rm --user "$(id -u):$(id -g)" -v "$KVM:/src:ro" -w /tmp "$IMG" sh -c '
set -u
cat > /tmp/mutations.txt <<"EOT"
Windows/kvm_hwtest_core.c @@ if (err.code != MFH264_E_ARG && !(rate && err.code == MFH264_E_CODECAPI)) { char r[40]; @@ if (0) { char r[40]; @@ any refusal of the open is taken as "another form"
Windows/kvm_hwtest_core.c @@ if (err.code == MFH264_E_HANG || err.code == MFH264_E_EXCEPTION) { char r[40]; @@ if (0) { char r[40]; @@ a hang in set_qp is taken as "another form"
Windows/kvm_hwtest_core.c @@ be->close(h); h = NULL;\t\t\t\t\t\t// this form @@ h = NULL;\t\t\t\t\t\t// this form @@ an encoder whose form was refused stays open
Windows/kvm_hwtest_core.c @@ if (info.max_inflight > 1) @@ if (info.max_inflight > 9) @@ a depth of 2 or more is accepted
Windows/kvm_hwtest_core.c @@ if (res->openMs > (uint32_t)KVM_HWT_OPEN_MS) @@ if (res->openMs > 99999u) @@ a slow open is accepted
Windows/kvm_hwtest_core.c @@ if (p->force && be->force_idr(h, &err) != 0) @@ if (0 && be->force_idr(h, &err) != 0) @@ the key frame is never asked for
Windows/kvm_hwtest_core.c @@ if (idr != p->expectIdr) { @@ if (0) { @@ the IDR flag of the encoder is not compared
Windows/kvm_hwtest_core.c @@ p->force ? "idr_late" : (idr ? "idr_unasked" : "idr_missing") @@ "idr_late" @@ the three IDR failures have one name
Windows/kvm_hwtest_core.c @@ if (idr && len > 1536u * 1024u) @@ if (idr && len > 15360u * 1024u) @@ a huge key frame is accepted
Windows/kvm_hwtest_core.c @@ if (len < 5) { @@ if (0) { @@ an empty access unit is accepted
Windows/kvm_hwtest_core.c @@ if (rc == MFH264_E_HANG) { snprintf(r, sizeof(r), "hang"); } @@ if (0) { snprintf(r, sizeof(r), "hang"); } @@ a hang has no name of its own
Windows/kvm_hwtest_core.c @@ if (chk.r[i].status == KVMH_FAIL) { set_rule @@ if (0) { set_rule @@ the rules of the viewer are not judged
Windows/kvm_hwtest_core.c @@ if (chk.haveSps && chk.sps.level > c->level) @@ if (0) @@ an SPS above the announced level is accepted
Windows/kvm_hwtest_core.c @@ kvmh_colour_verdict(&chk.extra) == KVMH_COLOUR_WRONG @@ 0 @@ the colour is not judged
Windows/kvm_hwtest_core.c @@ sumA * (unsigned long)nB * 100ul <= (unsigned long)KVM_HWT_QP_RATIO_X100 * sumB * (unsigned long)nA @@ sumA * (unsigned long)nB * 100ul < (unsigned long)KVM_HWT_QP_RATIO_X100 * sumB * (unsigned long)nA @@ the ratio has to be only not below 1.4
Windows/kvm_hwtest_core.c @@ sumA * (unsigned long)nB * 100ul <= (unsigned long)KVM_HWT_QP_RATIO_X100 * sumB * (unsigned long)nA @@ sumA * (unsigned long)nB * 100ul <= (unsigned long)KVM_HWT_QP_RATIO_X100 * sumA * (unsigned long)nB @@ the QP 40 sizes are not used
Windows/kvm_hwtest_core.c @@ if (res->p50Us > KVM_HWT_P50_US || res->p95Us > KVM_HWT_P95_US || idrMaxUs > KVM_HWT_IDR_US) @@ if (res->p95Us > KVM_HWT_P95_US || idrMaxUs > KVM_HWT_IDR_US) @@ the median is not judged
Windows/kvm_hwtest_core.c @@ if (res->p50Us > KVM_HWT_P50_US || res->p95Us > KVM_HWT_P95_US || idrMaxUs > KVM_HWT_IDR_US) @@ if (res->p50Us > KVM_HWT_P50_US || idrMaxUs > KVM_HWT_IDR_US) @@ the 95th percentile is not judged
Windows/kvm_hwtest_core.c @@ if (res->p50Us > KVM_HWT_P50_US || res->p95Us > KVM_HWT_P95_US || idrMaxUs > KVM_HWT_IDR_US) @@ if (res->p50Us > KVM_HWT_P50_US || res->p95Us > KVM_HWT_P95_US) @@ the key frame time is not judged
Windows/kvm_hwtest_core.c @@ if (res->verdict == KVM_HWP_V_FAIL) { return; } @@ ; @@ a later SLOW or a later failure overwrites the first failure
Windows/kvm_hwtest_core.c @@ if (verdict == KVM_HWP_V_SLOW && res->verdict == KVM_HWP_V_SLOW) { return; } @@ ; @@ the first SLOW rule is replaced by the next
Windows/kvm_hwtest_core.c @@ if (h != NULL) { be->close(h); } @@ ; @@ the encoder is not closed at the end
Windows/kvm_hwtest_core.c @@ res->qpForm = usedForm; @@ ; @@ the form that worked is not told
Windows/kvm_hwtest_core.c @@ p[n++] = (frame_plan){ K_SCROLL, 8, 30, 1, 1, 0 }; @@ p[n++] = (frame_plan){ K_SCROLL, 8, 30, 0, 0, 0 }; @@ the key frame is not asked for in the plan
Windows/kvm_hwtest_core.c @@ p[n++] = (frame_plan){ K_SCROLL, 4, 40, 0, 0, 0 }; @@ p[n++] = (frame_plan){ K_SCROLL, 4, 40, 0, 0, 40 }; @@ the picture after the QP change is measured
Windows/kvm_hwtest_core.c @@ p[n++] = (frame_plan){ K_SCROLL, 0, 26, 0, 0, 0 }; @@ p[n++] = (frame_plan){ K_SCROLL, 0, 26, 0, 0, 26 }; @@ the picture after the QP change is measured (26)
Windows/kvm_hwtest_core.c @@ rc = encode_frame(be, h, nv12 != NULL && i < nPlan - 2, @@ rc = encode_frame(be, h, 0, @@ the frames of the plan never go by the view
Windows/kvm_hwtest_core.c @@ rc = encode_frame(be, h, nv12 != NULL && i < nPlan - 2, @@ rc = encode_frame(be, h, nv12 != NULL, @@ every frame of the plan goes by the view (the BGRA way is not in the stream)
Windows/kvm_hwtest_core.c @@ rc = encode_frame(be, h, nv12 != NULL, bgra, nv12, @@ rc = encode_frame(be, h, 0, bgra, nv12, @@ the flat key frame goes as BGRA
Windows/kvm_hwtest_core.c @@ if (kvm_yuv_bgra_to_nv12(bgra, w, hh, w * 4, nv12, w, hh, KVM_YUV_AUTO) != 0) @@ if (0) @@ the picture is not converted for the view
Windows/kvm_hwtest_core.c @@ rc = be->encode_nv12(h, nv12, (size_t)w * (size_t)hh * 3u / 2u, @@ rc = be->encode_nv12(h, nv12, (size_t)w * (size_t)hh, @@ the view is given with the size of the luma only
Windows/kvm_hwtest_core.c @@ if (rc == 0 && idr && len >= 5) { res->flatMilli @@ if (idr && len >= 5) { res->flatMilli @@ the floor is taken from a flat frame that failed
Windows/kvm_hwtest_core.c @@ : -1;\n\t\tif (rc != 0 && (rc == MFH264_E_HANG @@ : -1;\n\t\tif (0 && (rc == MFH264_E_HANG @@ a hang or an exception of the key frame request of the flat picture is let go
Windows/kvm_hwtest_core.c @@ \t\t\tif (rc != 0 && (rc == MFH264_E_HANG @@ \t\t\tif (0 && (rc == MFH264_E_HANG @@ a hang or an exception on the flat picture is let go
Windows/kvm_hwtest_core.c @@ err.code == MFH264_E_EXCEPTION)) { set_rule(res, KVM_HWP_V_FAIL, "flat"); goto done; }\n\t\tif (rc == 0) @@ err.code == MFH264_E_EXCEPTION || 1)) { set_rule(res, KVM_HWP_V_FAIL, "flat"); goto done; }\n\t\tif (rc == 0) @@ any error of the key frame request of the flat picture is a failure
Windows/kvm_hwtest_core.c @@ (strcmp(res->rule, "qp") == 0 || strcmp(res->rule, "qp_form") == 0) @@ (strcmp(res->rule, "qp") == 0) @@ an encoder that takes no QP form at all does not get the pass "rate"
Windows/kvm_hwtest_core.c @@ (strcmp(res->rule, "qp") == 0 || strcmp(res->rule, "qp_form") == 0) @@ (0) @@ there is no pass "rate"
Windows/kvm_hwtest_core.c @@ *res = r2; @@ ; @@ the result of the second pass is dropped
Windows/kvm_hwtest_core.c @@ usedForm = fi == 0 ? KVM_HWP_FORM_BR_CBR : KVM_HWP_FORM_BR_LDVBR; @@ usedForm = KVM_HWP_FORM_BR_CBR; @@ the form 9 is never told
Windows/kvm_hwtest_core.c @@ !(rate && err.code == MFH264_E_CODECAPI) @@ 1 @@ a refusal of CBR is a failure and not the turn of LowDelayVBR
Windows/kvm_hwtest_core.c @@ for (fi = 0; fi < (rate ? 2 : 3) && h == NULL; fi++) @@ for (fi = 0; fi < (rate ? 1 : 3) && h == NULL; fi++) @@ LowDelayVBR is never tried
Windows/kvm_hwtest_core.c @@ cfg.rc = fi == 0 ? MFH264_RC_CBR : MFH264_RC_LDVBR; @@ cfg.rc = MFH264_RC_LDVBR; @@ CBR is never the first mode
Windows/kvm_hwtest_core.c @@ (rate ? be->set_bitrate( @@ (0 ? be->set_bitrate( @@ the bit rate is never changed in the pass
Windows/kvm_hwtest_core.c @@ : 1000000ull); @@ : 8000000ull); @@ the bit rate of "QP 40" is the one of "QP 26"
Windows/kvm_hwtest_core.c @@ rate ? "rate" : "qp" @@ "qp" @@ the rule of the second pass is called qp
Windows/kvm_hwtest_core.c @@ b = b * (unsigned long long)c->codedW * (unsigned long long)c->codedH / (1920ull * 1088ull); @@ ; @@ the bit rate is not scaled by the area
Windows/kvm_hwtest_core.c @@ return (int)(b > mx ? mx : b); @@ return (int)b; @@ the bit rate is not limited by the level
EOT
survived=0; invalid=0; killed=0; n=0
while IFS= read -r line; do
  [ -z "$line" ] && continue
  n=$((n + 1))
  file=$(printf "%s" "$line" | awk -F " @@ " "{print \$1}"); search=$(printf "%s" "$line" | awk -F " @@ " "{print \$2}"); replace=$(printf "%s" "$line" | awk -F " @@ " "{print \$3}"); desc=$(printf "%s" "$line" | awk -F " @@ " "{print \$4}")
  rm -rf /tmp/m && mkdir /tmp/m && cp -r /src /tmp/m/kvm
  S="$search" R="$replace" perl -0pi -e "BEGIN { \$s = \$ENV{S}; \$r = \$ENV{R}; \$s =~ s/\\\\n/\\n/g; \$r =~ s/\\\\n/\\n/g; \$s =~ s/\\\\t/\\t/g; \$r =~ s/\\\\t/\\t/g; } \$c += s/\\Q\$s\\E/\$r/; END { exit(\$c >= 1 ? 0 : 3) }" /tmp/m/kvm/$file
  if [ $? -ne 0 ]; then echo "INVALID  ($desc): the text was not found"; invalid=$((invalid + 1)); continue; fi
  cd /tmp/m/kvm/Windows/tests
  SRC="kvm_hwtest_core_test.c ../kvm_hwtest_core.c ../../common/kvm_hwpolicy.c ../../common/kvm_synth.c ../../common/kvm_yuv.c ../../common/kvm_h264_rules.c ../../common/kvm_annexb.c ../../common/kvm_video_core.c ../../common/kvm_stream.c"
  if ! gcc -std=gnu99 -O1 -w -I.. -o t $SRC -lm 2>/tmp/m/err; then echo "INVALID  ($desc): does not compile"; invalid=$((invalid + 1)); cd /tmp; continue; fi
  if ./t >/tmp/m/out 2>&1; then echo "SURVIVED ($desc)"; survived=$((survived + 1)); else echo "killed   ($desc): $(grep -m1 FAIL /tmp/m/out | cut -c1-90)"; killed=$((killed + 1)); fi
  cd /tmp
done < /tmp/mutations.txt
echo "mutations: $n, killed $killed, survived $survived, invalid $invalid"
[ $survived -eq 0 ] && [ $invalid -eq 0 ]
' sh
