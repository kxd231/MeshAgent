# Windows KVM performance changes (branch `kvm-perf`)

Based on upstream `26846b1c7f`. Only the Windows KVM (`meshcore/KVM/Windows`) and the
x64 configurations of `meshservice/MeshService-2022.vcxproj` are changed; the protocol
and the server are unchanged.

1. `kvm.c`: the capture loop runs with `timeBeginPeriod(1)`, so the frame-rate
   pause is not rounded up to the 15.6 ms system tick (links `winmm.lib`). When tiles
   were sent, the frame work time is subtracted from the frame-rate timer.
2. `tile.cpp`: the capture bitmap is reused between frames. When scaling, the
   screen is captured at full resolution into a DIB section (no `GetDIBits`) and
   downscaled on the CPU with an area-average filter: SSE2 fast paths for 50% (2:1)
   and 75% (4:3), a table-based filter for other ratios. The visible screen area is
   mapped to the visible scaled area (`StretchBlt` mapped the tile-padded sizes,
   stretching the image vertically by ~1%). `StretchBlt` is only a fallback;
   `KVM_STRETCH_MODE` selects its mode (default `HALFTONE`).
3. `tile.cpp`: on x64, JPEG tiles (types 1 and 4) are encoded with the
   TurboJPEG 3 API directly from the desktop buffer instead of GDI+.
   PNG/TIFF and other pixel formats still use GDI+. `KVM_TJ_SUBSAMP` selects
   chroma subsampling (default `TJSAMP_444`, sharp colored text).
4. `kvm.c`: the screen is captured on a separate thread. GDI capture is paced by
   DWM composition (frame times are multiples of 16.7 ms), so while the screen is
   changing the next frame is captured while the current one is encoded and sent.
   When nothing changes the loop stays sequential (same idle CPU use and latency).
   The geometry (resolution, scaling, desktop) is only changed by the main loop while
   the capture thread is idle; frames captured with an older geometry are dropped.

Measured on a Ryzen 5 5500U laptop, 2160x1440 at 60 Hz, JPEG q70, frame-rate
timer 20 ms, screen changing continuously:

| scaling | upstream | `kvm-perf` |
|---|---|---|
| 100% | ~14.5 FPS | 30 FPS |
| 75%  | ~12 FPS   | 30 FPS |
| 50%  | ~15 FPS   | 30 FPS |

30 FPS is the limit of GDI capture on that machine (`BitBlt` takes ~28-31 ms).

## libjpeg-turbo

The library is not part of this repository. Install/extract the official
libjpeg-turbo 3.x **VC x64** build
(https://github.com/libjpeg-turbo/libjpeg-turbo/releases) so that these exist:

    C:\libjpeg-turbo64\include\turbojpeg.h
    C:\libjpeg-turbo64\lib\turbojpeg-static.lib

A different location can be passed with `msbuild /p:TurboJpegDir=<dir>`.
Only the `Release|x64` and `Debug|x64` configurations of
`MeshService-2022.vcxproj` use it (they define `_TURBOJPEG`).

## Build

    msbuild MeshAgent-2022.sln /m /t:MeshService-2022 /p:Configuration=Release /p:Platform=x64

The output is `Release\MeshService64.exe` (not signed). Compile-time options can be
passed through the `CL` environment variable, e.g. `set CL=/DKVM_TJ_SUBSAMP=TJSAMP_420`.

## Deploying to a MeshCentral server

Put the binary in `meshcentral-data/agents/MeshService64.exe` and restart the server;
agents with automatic updates enabled update themselves. The server then serves this
file instead of its bundled agent (and no longer offers official agent updates) until
the file is removed.

## Baseline measurement for the DXGI work (WA-0, build `kvm-perf-2`, commit `0a1ef10`)

Tool: `test/docker/run-kvm-bench-agent.sh` in the MeshCentral test tree (branch `wp/wa-kvm-agent`, report `docs/kvm-dev/wa-0.md`).
One generator window (1280x720, at the screen origin, no file written, closes itself) per run, one new KVM session per run, 10 s measured, no CPU throttling;
session settings as in WP-K1: JPEG quality 80, 100% scaling, 30 ms frame timer; the client script is fixed, only the agent changes between series.
The generator draws a frame counter, a phase and an echo counter as black/white 8x8 px cells into the first two 32x32 px tiles, so frames per second counts
*distinct screen states* that reached the browser. Scenarios: `static` (nothing changes), `typing` (one character into a text box, 12/s), `scroll` (a text box scrolled
one line, 30/s), `video` (1280x688 area repainted 60/s: rotating gradient, three translucent moving discs, a sweeping band; every pixel changes every frame).
Generator alone (no KVM session) reached 59.6 / 29.9 / 11.9 frames/s for video / scroll / typing, also while KVM was running, so it does not limit the measurement.

Win10 LTSC 21H2 virtual machine (WinBoat-style container, 4 GB RAM, 2 cores, no GPU, screen 1280x720; on a host with a load average of 2-5 shared with other test VMs),
agent file sha256 `C3012C1E4A75...`, n = 3 runs per cell, mean (min-max):

| metric | static | typing | scroll | video |
|---|---|---|---|---|
| KVM data, KiB/s | 0.94 (0.93-0.94) | 20.0 (19.9-20.1) | 444 (432-453) | 1037 (921-1099) |
| frames/s (distinct states) | - | 11.9 (11.87-11.89) | 26.2 (25.4-26.7) | 26.4 (23.5-27.9) |
| generated frames/s | - | 11.9 | 29.9 | 59.8 |
| gap between frames: mean / p95 / max, ms | - | 83 / 128 / 139 | 38 / 64 / 87 | 38 / 54 / 98 |
| CPU of the -kvm child, % of one core | 8.6 (7.5-9.5) | 11.6 (10.6-12.2) | 16.3 (14.8-17.5) | 25.5 (21.5-31.3) |
| CPU of the service, % of one core | 0.05 | 0.47 | 1.5 | 2.4 (0.9-4.1) |
| input -> screen echo, median / p95, ms | 42 / 60 | 40 / 63 | 32 / 69 | 36 / 73 |
| frame delay above its minimum, median / p95, ms | - | 25 / 60 | 23 / 31 | 12 / 22 |

A/A noise (video, two series of 3 runs, minutes apart): bandwidth +4%, frames/s +5%, KVM CPU -10%; coefficient of variation over the 6 runs: bandwidth 6%, frames/s 6%,
gap mean 7%, gap p95 11%, KVM CPU 14%, echo median 32%, echo p95 21%, frame delay median 14%. Differences below these figures are noise on this setup.

Observed ceiling: 26-28 frames/s with a 60/s source and 30 ms frame timer (the GDI `BitBlt` + CRC + encode loop), consistent with the ~30 FPS limit of GDI capture noted above.

GDI object leak seen while measuring (not fixed here): the `-kvm` child's GDI object count (`GetGuiResources`) grows ~60 per second in a session in which no remote mouse
move has happened yet (412 -> 1669 in 20 s; 412 -> 9999 in 200 s, i.e. the 10 000 per process limit) and ~6 per second after remote mouse moves (+57 in 10 s). In a 200 s
video run the picture and the process kept working at the limit (27.8 frames/s to the end, same PID), so the limit alone did not break capture on this machine.

## 5. DXGI Desktop Duplication (WA-1)

Only `meshcore/KVM/Windows` and the x64 configurations of `meshservice/MeshService-2022.vcxproj` (plus the
workflow `.github/workflows/kvm-perf-release.yml`) are changed; the protocol and the server are not.

**Behaviour.** On x64 (`MeshService-2022`, `Release|x64` and `Debug|x64`, macro `KVM_DXGI`) the capture thread keeps the screen in the
full resolution bitmap and updates it from `IDXGIOutputDuplication`: only the rectangles that DXGI reports as changed (and the
destinations of moved rectangles) are copied from the GPU, through a staging texture or, when the driver keeps the desktop image in
system memory (`DesktopImageInSystemMemory`, e.g. virtual machines), straight from the mapped surface. The result is a normal BGRA
buffer, so scaling and tile encoding are unchanged. A frame is `FULL` (every tile goes through the CRC check, as with GDI), `PARTIAL`
(only the tiles that touch the changed rectangles are checked) or `UNCHANGED` (nothing is copied, compared or encoded; the loop waits
in `AcquireNextFrame`, so an idle screen costs almost no CPU). Moved rectangles are only counted and logged, they are copied as ordinary changes (no CopyRect).
The mouse cursor is not part of the DXGI image: at 100 % it is drawn into the output like the GDI path does (`mouseMove[0] != 0`),
otherwise the cursor messages of the protocol are used as before.
`DXGI_ERROR_INVALID_CALL` from `AcquireNextFrame` (seen on Hyper-V after a mode change) is a transient failure (no penalty, 500 ms); three in a row without an image count as an error. GDI is used automatically when: `DuplicateOutput` fails or `d3d11.dll`/`dxgi.dll` are missing (they are loaded at run time, nothing is added
to the imports), Windows 7 (no `IDXGIOutput1`), the capture rectangle is not exactly one output (all screens of several monitors), a rotated output,
scaling up, and after `DXGI_ERROR_ACCESS_LOST` until the duplication is created again (250 ms; more than 5 times a minute: 5 s). Three failures in a row on
the default desktop switch DXGI off for the rest of the process; a failure on another desktop (logon screen, UAC) is not counted, DXGI is tried again when the
desktop changes. DXGI calls run under `__try/__except`: an exception switches DXGI off for the process.

**Switches.** `/DKVM_DXGI_DISABLE` (compile time, e.g. `set CL=/DKVM_DXGI_DISABLE`, workflow input `extra_cl`) builds the GDI path (the log stays);
`MESH_KVM_FORCE_GDI=1` in the environment of the service (the KVM child inherits it: `HKLM\SYSTEM\CurrentControlSet\Services\<service>\Environment`,
REG_MULTI_SZ, then restart the service) forces GDI at run time.

**Log.** `<directory of the exe>\<exe name without .exe>-kvm.log`, one line per event and one `stats` line per 10 s (limit 1 MB, then nothing is appended).
**Every x64 build of this branch writes this log** (`KVM_STATS_LOG` is defined together with `KVM_DXGI` in `Debug|x64` and `Release|x64` of `MeshService-2022.vcxproj`, also with `/DKVM_DXGI_DISABLE`);
**before a production release the log has to be switched off**: remove `KVM_DXGI` from `PreprocessorDefinitions` (this also removes DXGI) or, to keep DXGI without the log, stop defining `KVM_STATS_LOG` in `dxgi_capture.h` (the code guarded by it, including the `stale` check, is then not compiled). Lines:
`start build=... dxgi=<0|1> env_force_gdi=<0|1>`, `dxgi open adapter=... mode=... sysmem=<0|1> desktop=<name>`, `dxgi lost|fail|ineligible|disabled|slow_init|lost_storm|stale...`,
`stats t= period_ms= path=<dxgi|gdi|mixed> frames= full= partial= unchanged= fps= ... copy_us_avg= out_us_avg= capture_us_avg= write_ms= dirty_rects= move_rects= ...`.
`frames` counts every iteration of the loop (also `unchanged`): the picture rate is `fps = (full + partial) / period`. `copy_us`: GPU -> persistent image,
`out_us`: copy of the whole image and the cursor (or scaling) into the frame buffer, `write_ms`: time blocked in the write to the pipe.
Every 10 s, when DXGI reported no change for a second, the persistent image is compared with a fresh GDI capture outside the cursor (`stale_*`);
two differences in a row (`dxgi stale`) re-create the duplication; if the difference is still there after that (`dxgi stale_systematic`, e.g. HDR) the check only measures and logs for the rest of the process.

**Tool.** `meshcore/KVM/Windows/tools/kvm_dxgi_check.cpp` (built by the workflow as `kvm-dxgi-check.exe`, not part of the agent): `info`, `compare`, `rects`, `cursor [--ibeam]`, `pointer [seconds] [--move]`, `idle`, `stalecost`.

**Software cursor in the DXGI image.** On some virtual machines (Windows 10 LTSC on a VirtIO GPU display driver or the Microsoft Basic Render Driver, `sysmem=1`) the system composes the
mouse cursor into the desktop image that Desktop Duplication delivers; the GDI path never contained it. The viewer draws its own pointer, so it showed twice. Not every machine without a GPU does it
(a Windows 11 Hyper-V machine with the same adapter does not), so the detection does not look at the adapter. `kvm-dxgi-check pointer [seconds] [--move]` showed what DXGI says
(`PointerPosition.Visible` with `LastMouseUpdateTime != 0` is **not** a usable signal): with a software cursor the duplication reports no pointer at all (no frame with `LastMouseUpdateTime != 0`, also while the
cursor moves) and the cursor field differs from GDI by 140-290 pixels; with a hardware cursor (Laptok) or a hypervisor that delivers the pointer (win11-test) pointer updates with `Visible = TRUE` arrive within a second and the field is identical to GDI
(`Visible = FALSE` updates also occur on hardware while `CURSOR_SHOWING` is set, so that combination says nothing).
*Detection:* the agent latches "hardware" at the first pointer update with `Visible = TRUE` (nothing is examined or done until the duplication is opened again); until then, for every image frame with the
cursor showing at a position or with a handle not examined yet, the cursor field (cursor bitmap at its hotspot plus `KVM_DXGI_POINTER_MARGIN` = 24 px) of the persistent image is compared with a GDI read, and at least
`KVM_DXGI_POINTER_MIN_DIFF` = 8 differing pixels mean "embedded".
*Taking the cursor out without reading the screen per frame* (a GDI read of the screen costs 16-30 ms on these machines whatever its size: `kvm-dxgi-check bltcost`, 80x80 field 20 ms, whole 1280x720 screen 28 ms;
a version that read the field after every frame lost 39 % of the pictures/s in the `video` scenario). Invariant: the persistent image never holds the cursor. A hook in `dxgi_capture_acquire` (`dxgi_capture_set_precopy`) lets the agent save the cursor field of
the persistent image (S) just before a frame that touches it is copied in; afterwards the pixels under the shape of the cursor are put back from S, the rest of the field keeps the new DXGI pixels. The shape is a mask per cursor handle (8 handles cached):
the pixels that differ between a DXGI image and a GDI read, widened by one pixel (`dxgi_ptrmask.h`, host test `tools/test_ptrmask.cpp`). Where the content under the cursor changed, those pixels are out of date; a GDI read of the field (`SRCCOPY`) puts the truth
under the mask `KVM_DXGI_POINTER_QUIET_MS` (150) after the last frame that touched the field, or every `KVM_DXGI_POINTER_REFRESH_MS` (500) while frames keep touching it (video under the cursor); a read in a quiet moment also adds what the mask missed
(shadow, animated cursors) and replaces a provisional "whole field" mask (more than 60 % of the field, or a field cut by the screen edge, or content changing during the first read). A cursor handle without a mask costs one GDI read.
A moving cursor over static content, and a still screen, need no GDI read. The hardware path (win11-test, Laptok) does no extra work per frame.
Log: `dxgi pointer embedded=<0|1> reason=probe|probe_clean|hardware_pointer ...`, `dxgi pointer mask reason=probe|new_cursor|quiet ... pixels=`, and in `stats`: `ptr_embedded= ptr_restores= ptr_gdi_reads= ptr_gdi_us_avg= ptr_mask_px=`.
`/DKVM_DXGI_POINTER_KEEP` switches it off (the detection stays). Things to know: (1) the GDI read must not use `CAPTUREBLT`: on the Basic Render Driver every such read made the system draw the cursor again, a new DXGI image
for every read (13 images/s from a still screen, `kvm-dxgi-check pointer 5 --blt=capture`; `--blt=plain` gives none); without it layered windows (shadows, fading menus) that overlap the cursor field are missing from the pixels under the cursor until DXGI reports the area again.
(2) While content changes under a stationary cursor, the pixels under its shape are up to 500 ms out of date (a cursor-shaped patch of the older picture). (3) A frame composed with the cursor in A while the field is taken at B leaves a trace in A until the next frame (which reports A as changed); no trace outlives the last frame.
(4) The cursor drawn by the agent itself for the legacy overlay (XOR pointer, `mouseMove[0] != 0`) is unchanged; it is drawn into the copy of the image. (5) The state is lost when the duplication is closed; a cursor that is hidden or on another monitor is not handled.
Measured on the Windows 10 VM (1280x720, `video`, n = 3 x 10 s): 31.34 pictures/s (31.27-31.39), 787 KiB/s, echo 57 ms; GDI reads 13-15 per 10 s (28-30 ms each, about 4 % of the time), 150-180 restores per 10 s.
*A moving cursor (found in the frame, not asked for).* The system composes the cursor into a frame at the place where the pointer was when the frame was made; `GetCursorInfo`, asked afterwards, already gives a newer
place (measured with `kvm-dxgi-check trail --move`, a pointer moved every 10 ms on a 1280x720 virtual machine: in 124 of 125 frames the composed cursor was **not** where `GetCursorPos` said, it was 1-3 steps (10-30 ms) behind).
Restoring the background at the place `GetCursorInfo` gives therefore left the cursor in the image while the pointer moved (the lower the frame rate, the longer it showed) and a trace that stayed after it stopped. The same measurement showed that
DXGI reports one small rectangle per frame (the visible shape of the cursor at its old and its new place together: 124 of 125 frames at most 48 pixels on a side, none above 96). So the hook (`dxgi_capture_set_precopy`) saves the pixels of every
small rectangle of the frame (and, in a large rectangle, windows of 48 pixels around the places where the cursor can be: the one `GetCursorInfo` gives and the last three where it was found), and after the copy the cursor is searched in those regions by
its fully opaque pixels (`kvm_cursor_sprite`: the cursor drawn over black and over white by `DrawIconEx`; `pm_find`, exact comparison with a strict start: found in 125 of 125 frames, 40 microseconds on average, 4000 positions; never two matches). The mask of a
colour cursor is made from its bitmap (every pixel with alpha > 0, widened by one pixel), so a new cursor handle needs no GDI read. Monochrome cursors (I-beam) cannot be found by colour; they keep the position of `GetCursorInfo` and the GDI mask.
If the cursor is not found although the frame touched the field at the `GetCursorInfo` position (`ptr_not_found`), the restore is done there and the field is marked stale (a GDI read in a quiet moment, as before).
Test: `rtrail` (kvm-bench-dxgi-correct.spec.js) moves the remote pointer every 10 ms for 4 s over a still picture and compares canvas snapshots taken during the motion with a clean one; `rtrailc` does it over a changing picture by searching the snapshots for the shape of the arrow.
Measured, agent `717a3dc`, Windows 10 VM 1280x720: `rtrail` 0 differing pixels in every snapshot and afterwards, at a frame timer of 30 and of 100 ms (the build before it: 429-1274 differing pixels in the snapshots and 1573 afterwards at 30 ms); `ptr_found` 207 of 212 frames
over moving content, search 140-230 microseconds per frame in `video` (up to 440 with a large rectangle), `video` 31.57 pictures/s (31.45-31.78).

Measured on a Hyper-V virtual machine without GPU (Windows 11 Pro 24H2, 1920x1080 at 30 Hz, 4 GB, 4 vCPU; DXGI adapter "Microsoft Basic Render Driver", system memory surface),
generator in its own window, JPEG q80, 100 %, frame timer 30 ms, 10 s per run, n = 3, mean (min-max). CPU is the `-kvm` child as a share of one core.

| scenario | `kvm-perf-2` (GDI) | WA-1 build, `MESH_KVM_FORCE_GDI=1` | WA-1 build, DXGI |
|---|---|---|---|
| static: CPU | 28.0 % (23.2-31.6) | 23.5 % (23.2-24.1) | **1.4 % (0.9-1.7)** |
| typing 12/s: pictures/s, CPU | 11.84, 39.6 % | 11.88, 31.9 % | 11.92, **7.0 %** |
| scroll 30/s: pictures/s, CPU, bandwidth | 16.0, 45.6 %, 276 KiB/s | 19.0, 36.5 %, 322 KiB/s | **23.7**, **22.3 %**, 404 KiB/s |
| video 60/s: pictures/s, CPU, bandwidth | 14.9, 52.3 %, 591 KiB/s | 18.6, 44.8 %, 746 KiB/s | **27.5**, **31.7 %**, 1123 KiB/s |

The virtual display of that machine refreshes at 30 Hz, so about 28-29 pictures/s is the ceiling there. Run-to-run noise on this machine: about 12 % for bandwidth and picture rate, about 25 % for CPU.
The GDI baseline above was measured on a quiet host, the middle column later, so read the middle column as the GDI control of the new build and the third as the result.

Second machine (Windows 10 LTSC 21H2 virtual machine, VirtIO GPU display driver without acceleration, 1280x720, 4 GB, 2 vCPU, shared and loaded host, same settings, n = 3, mean (min-max)):

| scenario | `kvm-perf-2` (GDI) | WA-1 build, DXGI |
|---|---|---|
| static: CPU | 8.6 % (7.5-9.5) | **0.8 % (0.2-1.4)** |
| typing 12/s: CPU | 11.6 % | **2.0 %** |
| scroll 30/s: pictures/s, CPU | 26.2, 16.3 % | 27.6, 11.4 % |
| video 60/s: pictures/s, CPU, bandwidth | 26.4 (23.5-27.9), 25.5 %, 1037 KiB/s | **31.5 (31.4-31.6)**, 24.6 %, 1349 KiB/s |

The picture rate there is limited by the frame-rate timer of the benchmark (30 ms, at most 33 pictures/s), not by the capture: `kvm-dxgi-check rects` reports 41 DXGI images/s on that machine.
The number of GDI objects of the `-kvm` process stays flat (6-8; the old build grew by about 4 objects/s with the cursor overlay), which was the leak fixed by the first commit of this branch.

**What is not verified.** All measurements above, and all tests of WA-1, were made on three virtual machines **without GPU, all of them with `DesktopImageInSystemMemory = TRUE` (`sysmem=1`)**:
the images were read from the mapped system memory surface. The GPU path (staging texture, `CopyResource`/`CopySubresourceRegion`, `Map` with `RowPitch`, the bounding-box copy) and the copying of the
destinations of moved rectangles were **never executed in practice** (`move_rects` was always 0). Before DXGI is used on machines with a GPU, run `kvm-dxgi-check compare` and `rects`
(`copy_path` must say `staging`, `bbox_tested` and `o2_tested` must be 1, `verified` 1) on such a machine.

## 6. Quality that depends on motion, sharpening and the congestion regulator (WA-2)

Files: `kvm_adapt.h`, `kvm_adapt.cpp` (new), hooks in `tile.cpp`, `tile.h`, `kvm.c`; the protocol and the server are not changed (a sharpened tile is an ordinary picture command).
Active only in the x64 `MeshService` build with TurboJPEG (`KVM_ADAPT_ENABLED` = `_TURBOJPEG` && `KVM_STATS_LOG` && not `KVM_ADAPT_DISABLE`), for JPEG types 1 and 4 with 24/32 bit pixels.
In every other build (x86, ARM64, MeshConsole, GDI+ without TurboJPEG) the functions are stubs: every tile is "full class", nothing is sharpened, the frame pause is the frame-rate timer, i.e. the behaviour of WA-1.

**Behaviour.**
1. *Motion class.* A tile whose content changed in at least `KVM_ADAPT_MOTION_STREAK` (2) frames in a row, each change at most `KVM_ADAPT_MOTION_MS` (250 ms) after the previous one, while at least
   `KVM_ADAPT_MOTION_TILES` (16) tiles changed in the previous frame, is sent with JPEG quality `min(quality of the user, KVM_ADAPT_Q_MOTION = 50)` and 4:2:0 chroma subsampling. Typing and a blinking cursor stay in the full class (sharp).
   `get_tile_at` joins neighbouring tiles into one rectangle only within one class; a changed neighbour of another class is marked for sending by itself (never "don't send": a change must not get lost).
2. *Sharpening.* A tile that was sent lossy (motion class) and has not changed for `KVM_ADAPT_SETTLE_MS` (150 ms) is encoded again from the current picture with the quality of the user and the normal subsampling, in runs of up to `KVM_ADAPT_SHARPEN_RUN` (8) tiles of a row,
   at most `KVM_ADAPT_SHARPEN_BYTES` (96 KiB) per frame (a quarter at regulator level >= 4, a half at levels 1-3). When the content of the screen has not changed for `KVM_ADAPT_SETTLE_MS` and a lossy tile is due, the capture is asked for an image (`KVM_CAPTURE_WANT_IMAGE`): it waits for a new frame as usual (`KVM_DXGI_TIMEOUT_MS`) and, if none comes, returns the unchanged persistent image with an empty change map, so that an idle screen is still sharpened (about 0.2-0.25 s after the last change of the screen). While the screen changes the flag is not set. A tile that changes again before it is sharpened is simply handled as a normal change.
3. *Congestion regulator* (one per KVM session of the process, i.e. one for all viewers of that desktop). The signal is the share of the time the loop is blocked in the write to the pipe, in windows of `KVM_ADAPT_WINDOW_MS` (500 ms): at or above `KVM_ADAPT_CONGEST_PCT` (15 %) the level goes up (max 5),
   after `KVM_ADAPT_CALM_WINDOWS` (3) windows at or below `KVM_ADAPT_CALM_PCT` (3 %) it goes down. Level 1-3: motion quality 40 / 30 / 20 (step `KVM_ADAPT_Q_STEP` 10, floor `KVM_ADAPT_Q_MOTION_MIN` 20), and a tile that is only sent again after a refresh (REFRESH, new geometry) is also sent in the motion class and sharpened later.
   Level 4-5: additionally the frame pause is multiplied by 2 / 4 (at most `KVM_ADAPT_MAX_PAUSE_MS` 250 ms) and, while the output of the loop exceeds `KVM_ADAPT_RATE_FRACTION_PCT` (70 %) of the rate that the link accepted while it was saturated (a token bucket, measured from the write-block windows), the pause is stretched, at most to `KVM_ADAPT_MAX_PAUSE_MS`. This does not cap the output at 70 %: the capture still runs at least 4 times a second and a debt of more than 2 s of tokens is forgiven, so that a full refresh can never be starved.

**Switches.** `/DKVM_ADAPT_DISABLE` (compile time; workflow input `extra_cl`) builds the WA-1 behaviour (the log stays). `MESH_KVM_ADAPT=0` in the environment of the service (same registry value as `MESH_KVM_FORCE_GDI`, then restart the service) switches it off at run time. Every threshold above can be overridden with `/D<NAME>=<value>`.

**Log** (same file as WA-1): `adapt start enabled=<0|1> reason=<on|env> q_user=.. type=.. q_motion=.. motion_ms=.. settle_ms=..` (`q_user` is the quality before the settings of the viewer arrive; the quality used is the live one),
`adapt level from=.. to=.. block=<x.y>% accept_kbps=.. q_motion=.. since_start_ms=..`, `adapt rate_cap kbps=..`, and one `adapt stats` line after every `stats` line:
`level_end level_max q_motion rects_motion rects_full bytes_motion bytes_full sharpen_rects sharpen_tiles sharpen_bytes want_image lossy_end settle_ms_avg settle_ms_max block_pct_max accept_kbps pause_mult rate_skips`
(a separate line instead of extra fields of `stats`, so that the WA-1 line keeps its format). With `MESH_KVM_ADAPT=0` the levels are still computed and logged, but have no effect.

**Measured** (same machines and settings as section 5: JPEG q80, 100 %, frame timer 30 ms, 10 s per run, n = 3, mean (min-max); control = the same binary with `MESH_KVM_ADAPT=0`, measured right after the series with WA-2 on, test server before its rebuild of 23:03 CET):

| machine / scenario | bandwidth, WA-2 on | bandwidth, control | change | pictures/s on / control | CPU of the KVM child on / control |
|---|---|---|---|---|---|
| Windows 11 VM, scroll | 227.6 KiB/s (220-234) | 392.1 (378-406) | **-42 %** | 22.4 (22.2-22.6) / 22.7 (21.4-23.7) | 21.2 % / 21.0 % |
| Windows 11 VM, video | 614.5 (611-618) | 1139 (1131-1150) | **-46 %** | 27.6 (27.4-27.8) / 27.5 (27.2-27.7) | 31.6 % / 33.8 % |
| Windows 11 VM, typing | 19.98 | 19.99 | 0 | 11.90 / 11.90 | 6.9 % / 8.3 % |
| Windows 11 VM, static | 0.94 | 0.97 | 0 | - | 1.7 % / 1.5 % |
| Windows 10 LTSC VM, scroll | 238.2 (236-241) | 459.9 (447-467) | **-48 %** | 26.2 (26.0-26.5) / 28.4 (27.6-28.8) | 9.1 % / 10.0 % |
| Windows 10 LTSC VM, video | 737.5 (726-745) | 1356 (1348-1363) | **-46 %** | 27.9 (27.7-28.2) / 31.5 (31.4-31.6) | 18.9 % / 21.9 % |
| Windows 10 LTSC VM, typing | 22.44 | 22.51 | 0 | 11.90 / 11.91 | 0.7 % / 1.2 % |

On the Windows 10 machine the number of generator frames that the viewer sees per second is 8-11 % lower with WA-2 (the ranges do not overlap, run-to-run noise there is about 4-6 %); the number of pictures the agent sends per 10 s is the same in both modes (`stats` lines), the cause is not isolated (see the report `docs/kvm-dev/wa-2.md`).
The idle sharpening works: `settle_ms_avg` 153-168 ms, after a scroll or a video the screen is back to full quality within about 0.2 s (see also the settle test in the report).

Physical laptop with a GPU (AMD Radeon, `sysmem=0`, 1920x1200 at 60 Hz, WA-2 on, no control measured): scroll 353 KiB/s at 28.0 pictures/s, video 749 KiB/s at **30.9** pictures/s (above 30: the frame timer allows about 33), CPU of the KVM child 24 % / 31 %. The first full image of a session (about 2.6 MB) is sent with the default quality 50 (the settings of the viewer arrive later) and is then sharpened step by step to the quality of the user: 0.77 s on average, up to 1.2 s on that machine. A single window of blocked pipe writes at the start no longer raises the regulator (two in a row are needed).

Constrained link (client behind a `tc` ingress policer, video, Windows 11 VM, test server relay in 1-to-1 mode): at **5 Mb/s** WA-2 produces about 600 KiB/s, fits the link, and the frame delay stays at a median of 14-17 ms (P95 0.6-1.5 s); without WA-2 the delay grows by about 650 ms per second (median 11 s after 30 s). At 3 Mb/s WA-2 alone is not enough (median 15-16 s against 20-22 s without it): the regulator raises its level up to 5 within 1-3 s of the first blocked write, but on that server the first blocked write comes only after 13-28 s (kernel buffers on the way: send buffer of the server towards the viewer, receive buffer from the agent, send buffer of the agent; 10-20 MB in total), and with a smaller surplus it never comes within 45 s. **Conclusion: with this server the congestion signal reaches the agent too late, so the regulator alone does not keep the delay low on links below about 5 Mb/s; what helps there is the lower bandwidth of the motion class. A signal from the viewer (command 90 of WA-3) or a multiplexer with ACK is needed.** The regulator also needs two consecutive overloaded windows to leave level 0, so the first full image of a session no longer lowers the quality of the start image.

**Not verified / limits.** x64 with TurboJPEG only (see above); in the GDI+ fallback the motion quality is not applied (GDI+ reads the global quality). One regulator for all viewers of the desktop: with several viewers the slowest one decides. The signal is the time blocked in the write to the pipe; whether that is a measure of the link of the viewer depends on the server (in the test server's 1-to-1 relay mode the server buffers several MB before it stops reading from the agent, so the first reaction comes late; see the report).
