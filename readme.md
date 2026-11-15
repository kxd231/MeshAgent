# MeshAgent (kxd231 fork)

A fork of [MeshAgent](https://github.com/Ylianst/MeshAgent), the agent of
[MeshCentral](https://github.com/Ylianst/MeshCentral) by Ylian Saint-Hilaire and contributors. It is the agent that
the [MeshCentral fork kxd231/MeshCentral](https://github.com/kxd231/MeshCentral) hands out to Windows x64 computers.

The changes are in the remote desktop (KVM) of the 64-bit Windows agent: how the screen is captured, compressed and
sent to the browser. Everything else (terminal, files, the connection to the server, the other platforms) is the
upstream agent.

This is not the official MeshAgent. Report problems of this fork in
[this repository](https://github.com/kxd231/MeshAgent/issues), not upstream. Security issues: see
[SECURITY.md](SECURITY.md).

## What is different in the Windows remote desktop

### A sharper picture with less data

- **Faster screen capture.** The agent asks Windows which parts of the screen changed (DXGI Desktop Duplication)
  and sends only those. When that is not possible (Windows 7, several monitors shown as one picture, a rotated
  screen, a laptop with the lid closed, a driver error) it switches by itself to the classic capture and tries the
  faster one again later.
- **Smarter JPEG.** Pictures are compressed with libjpeg-turbo. Areas that are moving get a lower quality, because
  the eye does not notice it in motion, and are sent again in full quality as soon as they stop, so text and icons
  end up sharp.
- **The viewer decides how much bandwidth to use.** The bandwidth limit chosen in the desktop settings of the
  browser (from 1 to 20 Mbit/s, or no limit) is respected: under the limit the agent first lowers the quality of
  moving areas, then the number of frames per second.
- **Up to 60 frames per second** when the viewer asks for it.

### H.264 video (optional)

In the desktop settings of the browser you can choose **Encoding: H.264 (video)**. The agent then sends the parts
of the screen that move (a video, scrolling, a window being dragged) as video, like a video call does, and keeps
everything else as sharp JPEG. When the movement stops, the moved areas are sent once more as JPEG, so text never
stays blurry. The aim is smooth motion on slow links at the same or lower bandwidth.

- It needs the MeshCentral fork as the server and a browser that can decode H.264 (Chrome, Edge and other
  browsers that pass the built-in check). JPEG stays the default; nothing changes for users who keep it.
- Video is used while one person is watching the device. When a second viewer connects, everybody gets JPEG,
  and video comes back when that viewer leaves.
- If anything goes wrong (the agent cannot encode, the browser cannot decode or keep up) the session continues
  in JPEG and the browser shows why.

### The graphics card does the work

Most graphics cards have a built-in video encoder that is much lighter on the processor than encoding in
software. The agent uses it when it can:

1. The first time a computer is asked for video, the session starts with the software encoder and, in the
   background, the agent runs a short self test of the graphics card encoder in a separate process. Nobody sees
   it and it does not delay the session.
2. If the test passes, the next sessions use the graphics card. The result is remembered; the test runs again when
   the graphics card, its driver or Windows changes, or when a new agent brings a new version of the test.
3. If the graphics card encoder misbehaves during a session, the agent switches to the software encoder in the
   same session and remembers the problem. After repeated problems it stops using the graphics card on that
   computer, so a bad driver is not tried again and again.

Tested so far on AMD (a laptop) and NVIDIA (GeForce RTX 2060 SUPER) graphics. **Intel graphics have not been
tested yet**: the self test decides there too, and the expected worst case is that the graphics card is not used
and the software encoder does the work.

### Auto frame rate

The frame rate list of the viewer has an **Auto** entry. With H.264 the agent picks 60, 30, 20, 15 or 10 frames per
second by itself, from how well the computer and the network keep up: it goes down quickly when the processor or
the link is the limit and goes up again carefully. With JPEG, or with an older agent, the browser picks the rate from
what it receives.

### Known limits

- Intel graphics encoders are not tested (see above).
- On NVIDIA the first full picture after a pause can be large, so it takes a moment longer on a slow link.
- After a short drop of the network, Auto can try a higher frame rate again a little too soon.
- H.264 is only in the Windows x64 agent. Linux is planned; other agents keep JPEG.
- Windows N editions need the Media Feature Pack for H.264.

## Settings for administrators

Nothing has to be configured. These switches are there for troubleshooting. Environment variables go into the
environment of the agent service (registry `HKLM\SYSTEM\CurrentControlSet\Services\<service name>\Environment`,
type `REG_MULTI_SZ`, one `NAME=value` per line) and need a restart of the service. The text files are placed next to
the agent executable (for example `C:\Program Files\Mesh Agent\MeshAgent-kvmhw-mode.txt` for `MeshAgent.exe`) and
are read at the start of every desktop session.

| What | How |
|---|---|
| Never use the graphics card encoder | `MESH_KVM_VIDEO_HW=0`, or the file `<agent name>-kvmhw-mode.txt` containing `sw` |
| No H.264 at all on this computer | `MESH_KVM_VIDEO=0` |
| Auto chosen by the browser only | `MESH_KVM_AUTO=0` |
| Classic screen capture only | `MESH_KVM_FORCE_GDI=1`, or the file `<agent name>-kvm-capture.txt` containing `gdi` |
| Bandwidth limit when the viewer sets none | `MESH_KVM_BUDGET_KBPS=<kbit/s>` |
| Diagnostic log | `MESH_KVM_STATS_LOG=1` writes `<agent name>-kvm.log` next to the agent (at most about 2 MB, then it rotates). Off by default. |

## Releases

Windows x64 builds are published as [releases](https://github.com/kxd231/MeshAgent/releases) of this repository.
Each one is built by GitHub Actions from a tag and contains `MeshService64.exe`, a text file with its size and
hashes, and `kvm-dxgi-check.exe`, a small tool that checks the screen capture of a computer. Published releases
cannot be changed. The MeshCentral fork pins the exact size and SHA-384 of the agent it hands out, so a server only
gives computers the file it was released with.

How releases are made: [docs/releases.md](docs/releases.md). Technical details and measurements of the remote
desktop: [meshcore/KVM/Windows/README-kvm-perf.md](meshcore/KVM/Windows/README-kvm-perf.md).

## License

Apache License 2.0, like upstream (see the headers of the source files). MeshAgent is © Intel Corporation and its
contributors; the original author is Ylian Saint-Hilaire.
