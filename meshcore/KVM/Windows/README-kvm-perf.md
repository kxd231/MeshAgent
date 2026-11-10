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

**Tool.** `meshcore/KVM/Windows/tools/kvm_dxgi_check.cpp` (built by the workflow as `kvm-dxgi-check.exe`, not part of the agent): `info`, `compare`, `rects`, `cursor [--ibeam]`, `idle`, `stalecost`.

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
