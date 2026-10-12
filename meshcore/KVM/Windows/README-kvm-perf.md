# Windows KVM performance changes (branch `kvm-perf`)

1. `kvm.c`: the capture loop runs with `timeBeginPeriod(1)`, so the frame-rate
   pause is not rounded up to the 15.6 ms system tick (links `winmm.lib`).
2. `tile.cpp`: the capture bitmap is reused between frames. When scaling, the
   screen is captured at full resolution into a DIB section (no `GetDIBits`) and
   downscaled on the CPU with an area-average filter; `StretchBlt` is only a fallback.
   The frame work time is subtracted from the frame-rate timer (`kvm.c`). `KVM_STRETCH_MODE`
   selects the `StretchBlt` mode used when scaling (default `HALFTONE`;
   `COLORONCOLOR` is faster but makes text unreadable at 75%).
3. `tile.cpp`: on x64, JPEG tiles (types 1 and 4) are encoded with the
   TurboJPEG 3 API directly from the desktop buffer instead of GDI+.
   PNG/TIFF and other pixel formats still use GDI+. `KVM_TJ_SUBSAMP` selects
   chroma subsampling (default `TJSAMP_444`).

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

    msbuild MeshAgent-2022.sln /m /p:Configuration=Release /p:Platform=x64

Compile-time options for measurements can be passed through the `CL`
environment variable, e.g. `set CL=/DKVM_STRETCH_MODE=COLORONCOLOR`
or `set CL=/DKVM_TJ_SUBSAMP=TJSAMP_420`.
