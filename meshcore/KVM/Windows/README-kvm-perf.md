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
**The log is off by default.** The statistics are always collected (the adaptive quality below depends on them), but the file is written only when `MESH_KVM_STATS_LOG=1` is in the environment of the service (system environment variable, then restart the service), or in builds with `/DKVM_STATS_LOG_ALWAYS`. Lines:
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
(shadow; not animated cursors: the mask and the sprite come from step 0 of the animation, so while a busy cursor animates its parts outside that mask can stay in the picture) and replaces a provisional "whole field" mask (more than 60 % of the field, or a field cut by the screen edge, or content changing during the first read). A cursor handle without a mask costs one GDI read.
Fixes after the review (WA-2b round): (1) the field of a *still* cursor that a frame touched is marked out of date also when nothing outside the mask changed (a change entirely under the shape is put right by the next GDI read: after `KVM_DXGI_POINTER_QUIET_MS` of calm or every `KVM_DXGI_POINTER_REFRESH_MS`); a moving cursor over static content still costs no read. (2) `pm_find` gives the best match of every saved rectangle and `pm_pick` takes the one nearest to the expected places (GetCursorInfo and the last places where it was found; a full match within `KVM_DXGI_POINTER_NEAR_PX` = 128 pixels ends the search; at most 600 000 positions per frame); the refresh of an out-of-date field no longer requires that GetCursorInfo points to the place of the last restore (the GDI read has no cursor in it, so what goes under the mask is true wherever the cursor is). (3) `pm_sprite_spread` is a permutation for every n (the stride is coprime with n). (4) the buffer N is written only in the branches that set the field; of the small rectangles of a frame the ones near the cursor windows are saved first (at most `PM_REGIONS` = 16 are saved).
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
its fully opaque pixels (`kvm_cursor_sprite`: the cursor drawn over black and over white by `DrawIconEx`; `pm_find`, exact comparison with a strict start: found in 125 of 125 frames, 40 microseconds on average, 4000 positions; no second match in the generator picture. A real picture can hold a second one, for instance the same arrow inside a screenshot shown in the frame: of several candidates the one nearest to where the cursor is expected wins, see the next paragraph). The mask of a
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

## 7. Bandwidth budget, motion frame limit and JPEG tables (WA-2b)

Files: `kvm_adapt.h`, `kvm_adapt.cpp`, hooks in `tile.cpp`, `tile.h` (field `deferred`), `kvm.c`. Same scope as WA-2 (x64 `MeshService` with TurboJPEG, JPEG types 1 and 4). The server is not changed; the protocol gets one optional field (bytes 10-11 of command 5, the budget chosen by the viewer, added at the owner's request, see "Who sets the budget"); a viewer that does not send it, and an agent that does not know it, work as before.
`/DKVM_ADAPT_DISABLE` and `MESH_KVM_ADAPT=0` switch all of it off together with WA-2.

**Budget.** The bytes written to the pipe in each 500 ms window of the regulator (the picture packets: motion rectangles, full rectangles, sharpening; the few small packets of cursor shapes and displays are not counted) are compared with `KVM_ADAPT_BUDGET_KBPS` (default 0 = no budget; see "Who sets the budget" below).
A ladder of steps B0..B7 sets the quality of the motion class, the scale of the picture and the frame limit of the motion class (the quality is never raised above the quality of the user):

| step | motion quality | scale of the picture (share of the viewer's scale) | motion frame limit |
|---|---|---|---|
| B0 | `KVM_ADAPT_Q_MOTION` (50) | 100 % | none (loop pace) |
| B1 | `KVM_ADAPT_BQ1` (40) | 100 % | none |
| B2 | `KVM_ADAPT_BQ2` (40) | 100 % | `KVM_ADAPT_BF2` (30 frames/s) |
| B3 | `KVM_ADAPT_BQ3` (30) | 100 % | `KVM_ADAPT_BF3` (30) |
| B4 | `KVM_ADAPT_BQ4` (25) | 100 % | `KVM_ADAPT_BF4` (20) |
| B5 | `KVM_ADAPT_BQ5` (25) | 100 % | `KVM_ADAPT_BF5` (15) |
| B6 | `KVM_ADAPT_Q_FLOOR` (20, the floor of every class) | 100 % | 15 |
| B7 | 20 | `KVM_ADAPT_SCALE_S1` (768 = 75 %; the lowest allowed is 70 %) | 30 |

Below B7 nothing more is lowered: the byte bucket (next paragraph) lets the motion frames out as often as the budget allows (a screen full of detail at 1 Mb/s: 1-2 frames per second). The scale steps 87.5 % and 70 % were measured and left out: only 75 % has the SSE2 scaler of the agent (3-5 ms for a 1920x1080 picture on a 2-core VM), 87.5 % and 70 % use the generic filter and cost 17-28 ms per picture, which limits the frames more than they save.

**The budget is a hard limit (byte bucket).** Every picture packet is paid from a bucket that is refilled at the budget rate (a burst of `KVM_ADAPT_BUCKET_BURST_MS` = 2 s, debt at most `KVM_ADAPT_BUCKET_DEBT_MS` = 3 s). A rectangle that is not small (more than `KVM_ADAPT_BUCKET_SMALL_TILES` = 8 tiles of 32x32) and finds the bucket empty is held back like a frame-limited one (the tiles are "deferred", looked at again in the next frames and sent when the bucket allows, in the quality of the step); small rectangles (typing, the cursor) pass while the debt is below one second of the budget. From step B3 on, every rectangle above 8 tiles is sent in the quality of the step even if its tiles are not "moving" (and sharpened later). `KVM_ADAPT_BUCKET_SHARPEN_PCT` (20 %) of the rate is reserved for sharpening, so that it always finishes; the part nobody uses goes to the motion. The ladder goes down when a window is above 110 % of the budget or when the bucket was empty for half of it (as many steps at once as the debt says, at most 3 per window; no second window needed when the debt is deep), up one step after 6 windows under the budget with a bucket that was almost never empty.

**Scale of the picture.** The agent keeps the scale the viewer asked for (command 5) apart from the share the budget asks for (`SCALING_FACTOR_NEW` = viewer scale x share, set in one place, `scale_update`). The viewer asks again after every change of the size of the picture; the same scale does not touch the share (neither does a viewer that sends back the scale it was given), another scale starts it again from 100 %. The scale changes only while the screen moves: at least `KVM_ADAPT_SCALE_DOWN_MS` (3 s) between changes down, `KVM_ADAPT_SCALE_UP_MS` (10 s) going up while moving, never within `KVM_ADAPT_SCALE_INPUT_MS` (1.5 s) of a mouse or keyboard command of the viewer (clicks around a change of the size would land beside), and not within `KVM_ADAPT_BUDGET_WARM_MS` after a REFRESH or a change of the scale (the full picture of a still screen is not movement). **Movement** means frames with at least `KVM_ADAPT_MOTION_TILES` (16) tiles that really changed (a caret, a clock, or a picture seen for the first time after a change of the size do not count). After `KVM_ADAPT_SCALE_RESTORE_MS` (3.5 s) without movement the picture goes back to the scale of the viewer at once, and the scale does not go down again for `KVM_ADAPT_SCALE_BAN_MS` (20 s; the bucket paces the frames meanwhile). The still picture is in the scale and quality of the viewer: it is sent in the quality of the step first and sharpened under the budget (a full 1920x1080 picture at 1 Mb/s takes about 12 s, at 100 kbit/s several minutes: sharpening always finishes, its reserved share keeps coming). At the end of a session the scale of the viewer is requested again. Log: `adapt scale mult=<1/1024> from=.. viewer_scale=.. step=B<n> calm=<0|1>`; `adapt stats` has `scale_mult`, `scale_changes`, `bucket_defers`, `bucket_tok_kb`, `bucket_forgiven_kb`.
*Held-back changes cannot starve, and the order is by age.* Every held-back tile carries the time it was first held back (`deferredAt`). Once the oldest has waited `KVM_ADAPT_BUCKET_AGE_MS` (1 s), held-back changes go out in the order of their age (a rectangle passes when it holds a tile held back no later than the oldest + `KVM_ADAPT_BUCKET_AGE_TOL_MS` = 100 ms), not in the order of the scan, which starts at the top left: without that, what keeps moving at the top of the screen was sent again and again before anything below it (measured: 1 Mb/s, heavy content on the whole screen, the centre of the screen refreshed 3 times in 35 s while the top refreshed 21 times). A new change waits behind the older ones and takes its place by its time. Small rectangles (up to `KVM_ADAPT_BUCKET_SMALL_TILES` = 8 tiles: typing, the cursor, small animations) have a reserved share of the rate (`KVM_ADAPT_BUCKET_SMALL_PCT`, 20 %; what they do not use goes to the motion) and do not wait in the queue of the large ones; the debt of that share is capped at half a second of it and what is above the cap is paid from the main bucket (every byte is accounted for: a flood of small detailed rectangles is limited to the budget as well; measured: 8 tiles of random pixels on every frame, 200 kb/s: 104 % and one window of 114 % before the change); a small rectangle passes on its share only while the debt of the main bucket is shallower than `KVM_ADAPT_BUCKET_SMALL_GATE_MS` (1 s of the budget); at a deeper debt (a flood of them) it is held back and takes its place in the queue by age like a large one, so a flood neither pushes the main bucket to its limit (bytes forgiven) nor starves the large ones. While changes are held back, sharpening has only its own reserved share (not the positive tokens of the main bucket). Under a budget a rectangle has at most `KVM_ADAPT_BUCKET_MAX_TILES` (64) tiles and no more than about half a second of the budget (by the size of the last ones; at least 4 tiles), so the bucket rations a large picture in pieces; the debt is capped at `KVM_ADAPT_BUCKET_DEBT_MS` and what the cap forgives is counted (`bucket_forgiven_kb`, 0 in all measurements). The burst of the bucket is `KVM_ADAPT_BUCKET_BURST_MS` (500 ms).
*Known limits of the scale.* (1) The viewer's echo of the scale it was given is recognised only within 5 s of the change; a viewer that deliberately chooses the scale the agent has just set within that time is taken for the echo (and later the agent returns to the previous scale of the viewer). (2) The scale can change while a mouse button is held without moving for more than 1.5 s (the release then lands beside); only mouse and key commands are tracked, not the state of the buttons.
*Known limits.* The fast scaler (SSE2) exists only for the result 75 % of the screen: with a viewer scale other than 100 % the multiplier 75 % gives another ratio and the generic filter (17-28 ms per 1920x1080 picture on a 2-core VM). Viewers other than the browser client, which send back a reduced scale as their own, were not tested (the agent ignores a command 5 whose scale equals the current result). The agent draws no cursor overlay at a scale other than 100 % (the browser draws its pointer), also when the budget has lowered the scale. Two viewers of one desktop share the scale and the budget (the last one wins).
*Known limits of very low budgets* (worked out from the code, not measured; the viewer's list starts at 1 Mbit/s). Worst case: 8 tiles of noise that change in every frame, about 30 KB a frame. (1) Below about 0.5 Mbit/s such an animation can take the whole budget: larger changes next to it wait as long as it runs, the limit is kept. (2) Below about 0.15 Mbit/s it can exceed the limit by a few to some ten percent, because bytes are forgiven at the debt floor of the bucket; `bucket_forgiven_kb` in the statistics shows them.

The budget is a second input next to the blocked-write time of WA-2: the motion quality is the lower of the two (`min(q of the congestion level, q of the budget step)`), the frame limit comes from the budget step (and the fixed limit below); the other effects of the congestion levels (frame pause, rate cap) are unchanged.
Down the ladder after `KVM_ADAPT_BUDGET_UP_WINDOWS` (2) windows above `KVM_ADAPT_BUDGET_UP_PCT` (110 %) of the budget; the first `KVM_ADAPT_BUDGET_WARM_MS` (3 s) of a session do not count (the full first image and its sharpening are not motion).
Back up one step after `KVM_ADAPT_BUDGET_DOWN_WINDOWS` (6, i.e. 3 s) windows below `KVM_ADAPT_BUDGET_DOWN_PCT` (100 %): the ladder tries the higher quality and falls back after two windows above 110 % if it does not fit (a try that fails soon doubles the patience, see below). The band 100-110 % is the hysteresis.
Version 1 of the ladder (down at 100 %, up only below 70 %) stayed on B2-B4 for a video whose steady output at B0 was 3.7-4.1 Mb/s, because the windows at the lower steps (3.0-3.4 Mb/s) never fell below 70 % of the budget.
A step that is given back and needed again within 10 s doubles the patience for the next recovery (6, 12, 24, 40 windows); after 30 s without a step back up (counted from the last return by one step) it is back at 6, so the quality does not oscillate. Sharpening of still tiles stays on (its bytes count in the window, the budget never switches it off);
typing, the cursor and small changes (not in the motion class) keep full quality and the loop pace.

**Who sets the budget.** The viewer chooses it (desktop settings: for example 10 Mb/s on a LAN, 4 Mb/s or less over a slow VPN). Precedence: value from the viewer > `MESH_KVM_BUDGET_KBPS` in the environment of the service (kbit/s, for tests) > `KVM_ADAPT_BUDGET_KBPS` (default 0 = no budget: without a viewer that sends the field the agent behaves as before WA-2b as far as the budget goes; the JPEG tables of mode 2 and the frame timer from 10 ms stay).
The log shows the source: `adapt start ... budget_kbps=<n> budget_source=<default|env|client>`, `adapt budget_set budget=<kbit/s> source=client` when the viewer changes it during a session, `budget_source` in `adapt stats`.
*Protocol.* Command 5 (`MNG_KVM_COMPRESSION`) has size 10: type (u8), quality (u8), scaling (u16), frame timer (u16), all after the 4-byte header. A viewer that offers a budget sends size **12**: bytes 10-11 = budget as u16, network byte order, in units of **100 kbit/s** (0 = no limit, 40 = 4 Mb/s, 100 = 10 Mb/s). `kvm.c` reads it with `if (size >= 12)` and calls `kvm_adapt_set_budget()` (a value written by the input thread, read by the loop at the next 500 ms window; the ladder restarts from B0; the same value again changes nothing). A command of size 10 (older viewer, thumbnails of several desktops) does not touch the budget.
Backward compatibility: older Windows agents and the Linux agent (not changed by this work; it reads `size >= 10` and ignores bytes 10-11) ignore the extra bytes. **The macOS agent rejects the whole command 5 when its size is not 6** (`mac_kvm.c`, `if (size != 6) break;`): that was already so for size 10, the new field does not change it. Only the Windows agent understands the field.
The budget of a viewer ends with its session (`kvm_adapt_session_end`): the next session in the same process starts from the environment / macro value unless its own viewer sends the field (a value that arrives between two sessions is kept for the next one). With two viewers of one desktop and no multiplexer in between, the budget of the last one that sent the field wins; thumbnails (size 10) never change it. The first 3 s of a session are not counted for the ladder, a REFRESH or a second viewer joining later is: a full picture can cost one step for a few seconds while the screen is still.

**Motion frame limit.** `KVM_ADAPT_MOTION_FPS` (default 0 = no fixed limit; `MESH_KVM_MOTION_FPS` overrides) and the limit of the ladder step; the stricter one applies. Rectangles of the motion class are encoded and written at most that many times per second (a frame may come up to half a loop period early, so that the average holds the limit);
rectangles of other classes go out in every frame. A held-back rectangle is not lost: its tiles keep the new CRC and get the flag `deferred`. The main loop looks at deferred tiles again in the next frame even if the capture does not list them as changed (`kvm.c`: the change map does not mark them "don't send"),
`kvm_adapt_want_image()` asks for an image while a deferred tile exists (like sharpening: the capture returns the unchanged persistent image with an empty change map when nothing new arrives), and the tile is sent in the motion class in the first frame the limit allows. A held-back frame counts as activity for the loop pace and the overlapped capture.

**JPEG tables.** `KVM_ADAPT_JPEG_OPT` (`MESH_KVM_JPEG_OPT`): 0 = standard Huffman tables, 1 = optimized tables (`TJPARAM_OPTIMIZE`, libjpeg-turbo 3.2.0) for the sharpening only, **2 = for every rectangle (default)**,
3 = sharpening and rectangles up to `KVM_ADAPT_JPEG_OPT_TILES` (16) tiles. Standard baseline JPEG, decoded by the browser unchanged. The gain comes mostly from the small rectangles of a moving picture: the standard tables cost about 420 bytes of header in every file.
Measured in the agent (Windows 10 VM, 2 cores, no budget, 60 s, frame timer 30 ms; mode 1 / 2 / 3): video 811 / 455 (-44 %) / 735 KiB/s at 31.8 pictures/s in all three, CPU of the KVM child 17.9 / 23.6 / 17.4 %, encode time per 32x32 tile 2 / 4 / 2 us;
scroll (text) 233 / 207 (-11 %) / 216 KiB/s at 26.7 pictures/s, encode time per tile 6 / 13 / 7 us. Host micro test (libjpeg-turbo, real frames): -42..-54 % for a 32x32 rectangle, -35..-52 % at 256x128 and -3..-47 % for the whole picture, encode time +3..+150 %; see the report.

**A frame without changed tiles** (the capture was asked for an image only to look again at held-back or lossy tiles) keeps the motion count of the last frame that changed something. Without that, the frame after it was taken for "no motion" and a moving picture was sent in full quality (q80, 4:4:4) for a moment.

**Frame timer.** The agent accepts a frame timer from 10 ms (was 20 ms) in both places of `kvm.c` (compression command and frame-rate command); only the Windows agent, Linux and macOS are unchanged.

**Stale check.** `dxgi stale_suspect|stale px=<n> box=<x0>,<y0>,<x1>,<y1>`: the rectangle around the differing pixels (x1, y1 exclusive, screen pixels, top-down); the check itself is unchanged.

**Log.** `adapt start` has `budget_kbps motion_fps jpeg_opt`; `adapt budget from=B<n> to=B<n> window_kbps budget_kbps q_motion fps_cap calm_windows since_start_ms` at every step; `adapt stats` has, appended:
`budget_kbps win_kbps_avg win_kbps_max bstep_end bstep_max bsteps q_motion_min q_motion_avg motion_fps_cap deferred_tiles deferred_rects enc_rects enc_us_rect enc_us_tile sharp_enc_rects sharp_enc_us_rect sharp_enc_us_tile loop_n loop_wait_us loop_proc_us loop_pause_us`
(`enc_*`: encode time of the rectangles of a frame, `sharp_enc_*`: of the sharpening; `_tile` = per 32x32 tile of the rectangle; `loop_*`: per iteration of the main loop, microseconds waiting for the capture, processing the frame and pausing).
