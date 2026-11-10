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
