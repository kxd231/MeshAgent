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
