// node analyze-capture.js <dir with stream-*.bin>   (repo at /src read-only). Parses the captured byte stream like the viewer's transport, validates every 243 with the reference
// validator (test/unit/helpers/kvm-h264-ref.js) and the viewer's parser (public/scripts/agent-desktop-h264.js analyse), and writes the Annex B of every generation to <dir>/h264/<phase>-gen<N>-<WxH>.h264
'use strict';
const fs = require('fs'), path = require('path');
const ref = require('/src/test/unit/helpers/kvm-h264-ref.js');
let H, av;
try { av = require('/src/public/rdp/codecs/avc420.js'); } catch (e) { console.log('avc420 not loadable:', e.message); }
try { H = require('/src/public/scripts/agent-desktop-h264.js'); } catch (e) { console.log('agent-desktop-h264 not loadable:', e.message); }
const dir = process.argv[2];
fs.mkdirSync(dir + '/h264', { recursive: true });
const u16 = (b, i) => (b[i] << 8) | b[i + 1], u32 = (b, i) => ((b[i] << 24) | (b[i + 1] << 16) | (b[i + 2] << 8) | b[i + 3]) >>> 0;
function closeOut() { if (out) { fs.closeSync(out.fd); } out = null; }
const total = {};
let sw = 0, sh = 0, last = null, curGen = null, out = null, cur = null, gens = 0;   // the state runs across the files (phases) of one tunnel
for (const f of fs.readdirSync(dir).filter((x) => /^stream-.*\.bin$/.test(x)).sort()) {
    const phase = f.slice(7, -4), buf = fs.readFileSync(path.join(dir, f));
    const r = { frames: 0, markers: 0, idr: 0, rej: {}, viewerErr: {}, bytes: 0, gaps: 0, seqJumpsOnP: 0, genChanges: [], sps: {}, maxFrame: 0, rects: 0, cmds: {}, resets: 0 };
    let p = 0;
    function closeOut() { if (out) { fs.closeSync(out.fd); } out = null; }
    while (buf.length - p >= 4) {
        const cmd = u16(buf, p), size = u16(buf, p + 2);
        if (cmd === 27) {
            const n = u32(buf, p + 4), tot = 8 + n;
            if (buf.length - p < tot) break;
            const m = buf.subarray(p, p + tot);
            if (u16(m, 8) === 243) {
                const v = ref.validate(243, m, { screenW: sw, screenH: sh });
                const flags = m[20], seq = u32(m, 12), gen = m[21];
                if (v) { r.rej[v] = (r.rej[v] || 0) + 1; }
                else if (flags & 2) { r.markers++; if (last !== null && seq !== ref.seqNext(last)) r.gaps++; last = seq; }
                else {
                    r.frames++; r.bytes += tot; if (tot > r.maxFrame) r.maxFrame = tot; r.rects += u16(m, 26);
                    const idr = (flags & 1) !== 0; if (idr) r.idr++;
                    if (ref.isGap(last, seq, idr)) { r.gaps++; }
                    last = seq;
                    const data = m.subarray(28 + 8 * u16(m, 26));
                    if (idr) { // SPS bytes: profile, constraint flags, level
                        for (let i = 0; i + 8 < data.length; i++) { if (data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 1 && (data[i + 3] & 31) === 7) { const k = `profile=${data[i + 4]} constraint=0x${data[i + 5].toString(16)} level=${data[i + 6]}`; r.sps[k] = (r.sps[k] || 0) + 1; break; } }
                    }
                    if (H && av) { try { const res = H.analyse(Uint8Array.from(data), av, u16(m, 22), u16(m, 24), idr, cur); if (idr && res.spsBytes) cur = { sps: res.spsBytes, id: res.info.id }; } catch (e) { const k = String(e.message || e).slice(0, 60); r.viewerErr[k] = (r.viewerErr[k] || 0) + 1; } }
                    if (idr && (curGen !== gen || out === null)) { closeOut(); gens++; curGen = gen; const name = `${dir}/h264/${phase}-gen${gen}-${gens}-${u16(m, 22)}x${u16(m, 24)}.h264`; out = { fd: fs.openSync(name, 'w'), name }; r.genChanges.push(path.basename(name)); }
                    if (out) fs.writeSync(out.fd, data);
                }
            } else r.cmds['j' + u16(m, 8)] = (r.cmds['j' + u16(m, 8)] || 0) + 1;
            p += tot;
        } else {
            if (size < 4) { console.log('bad size at', p); break; }
            if (buf.length - p < size) break;
            if (cmd === 7) { sw = u16(buf, p + 4); sh = u16(buf, p + 6); }
            if (cmd === 242) { last = null; }
            r.cmds[cmd] = (r.cmds[cmd] || 0) + 1;
            p += size;
        }
    }
    r.leftover = buf.length - p; r.screen = [sw, sh];
    total[phase] = r;
}
closeOut();
console.log(JSON.stringify(total, null, 1));
