# libva-v4l2

A VA-API backend for V4L2 stateful M2M codecs. It provides hardware video decoding and
H.264/HEVC encoding on:

- **Qualcomm SC8280XP** (Iris), tested on **Radxa Dragon Q8B**;
- **CIX CD8180 / Sky1** (Arm Linlon MVE v8, V4L2 driver `mvx`, Mali-G720 through
  Panthor), targeting **Orange Pi 6 Plus** with Ubuntu 26.04 and a Sky1 Linux 7.x kernel.

The project is experimental and targets these platforms; it is not a generic backend for
all V4L2 devices.

## Support

| Codec | Decoding | Encoding |
| --- | --- | --- |
| H.264 / AVC | Constrained Baseline, Main, High (8-bit) | Same profiles (8-bit) |
| H.265 / HEVC | Main, Main10 (8/10-bit) | Main (8-bit) |
| VP9 | Profiles 0 and 2 (8/10-bit) | — |

Supported video is progressive 4:2:0. Decoding has been tested with FFmpeg,
GStreamer, mpv, VLC, Chromium, and Kodi; H.264 encoding with FFmpeg, GStreamer,
and Sunshine. HEVC encoding is tested with FFmpeg and GStreamer.

- H.264 and HEVC encoding support CQP, CBR, and VBR, even dimensions from 128×128 to
  3840×2160, and I/P frames with one reference. I-frame requests produce IDR frames; B frames
  are unsupported.
  HEVC encoding currently uses Main tier; Main10 encoding is not implemented.
- Optional FastCV VPP provides CDSP scaling for linear NV12 with DMA-BUF support.
  Widths must be multiples of 16 and heights even. Color/bit-depth conversion
  and other VPP filters are unsupported.
- Compatible DMA-BUF paths avoid raw-frame copies. Applications that cache
  surfaces before decoding use a GPU copy by default.

## Performance

HEVC Main (8-bit) throughput on **Radxa Dragon Q8B — SC8280XP, Iris HFI Gen2**,
using FFmpeg 9.0.1 and GStreamer 1.28.6. Values are average frames per second.

| Pipeline | Operation | 720p (1280×720) | 1080p (1920×1080) | 4K (3840×2160) |
| --- | --- | ---: | ---: | ---: |
| FFmpeg VA-API | Decode | 1111 | 806 | 297 |
| FFmpeg VA-API | Encode | 1289 | 714 | 206 |
| GStreamer VA | Decode | 1088 | 823 | 278 |
| GStreamer VA | Encode | 1285 | 709 | 206 |
| GStreamer direct V4L2 | Decode | 1199 | 882 | 330 |
| GStreamer direct V4L2 | Encode | 1287 | 724 | 210 |

Synthetic I/P streams, GOP 60, VBR at 5 / 10 / 20 Mbps respectively. Encoding
includes CPU NV12 upload; FFmpeg encoding uses **`async_depth=4`**. Decoding waits
for completed frames without CPU download or rendering. Results are three-run
means, except 4K encoding, which uses one completed run; each run processes
9,000 / 6,000 / 2,400 frames respectively. Actual performance depends on content
and pipeline configuration.

## Requirements

- Qualcomm: Linux with the Iris stateful driver and MSM DRM support, plus the changes
  supplied in [patches/](patches/), unless already present in your kernel: decode-order
  output, larger CAPTURE pools, and encoder deblocking controls. The patches are
  references to integrate into your kernel.
- CIX Sky1 (Orange Pi 6 Plus): a kernel with the Sky1 patches (Panthor GPU support),
  the Linlon VPU module (`amvx`, e.g. from `sky1-drivers-dkms`) with its firmware, and
  Mesa ≥ 25.1 (Panfrost/Panthor). No kernel patch from [patches/](patches/) is needed.
- Access to the video devices and the DRM render node (`video` and `render` groups).
- A C++17 compiler, Meson ≥ 0.61, Ninja, pkg-config, and development libraries
  for libva, libdrm, EGL, GLES 3.2, and GBM.
- For VPP: system FastCV and FastRPC libraries with a working CDSP runtime.

## Build and install

The Debian package CI builds one ARM64 package on Ubuntu Noble, then checks
installation and driver loading on Debian Trixie, Ubuntu Noble, and Ubuntu
Resolute. Tag builds publish the same package and debug symbols to a draft
GitHub Release after all checks pass. Hardware codec testing is separate.

On Arch Linux:

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf libva libdrm mesa libglvnd
python3 packaging/make-dist.py
cd packaging/arch
makepkg -si
```

The [PKGBUILD](packaging/arch/PKGBUILD) packages the userspace driver. After
editing packaged sources, regenerate the archive, update `sha256sums` using
`makepkg -g`, and refresh `.SRCINFO` with `makepkg --printsrcinfo > .SRCINFO`.

Alternatively, build and install with Meson:

```sh
meson setup build --buildtype=release --prefix=/usr -Dlibdir=lib
meson compile -C build
sudo meson install -C build
```

FastCV VPP defaults to `-Dfastcv=disabled`. Use `-Dfastcv=auto` to enable it
when dependencies are available or `-Dfastcv=enabled` to require build dependencies.
Debian / Ubuntu packages explicitly disable FastCV for consistent distribution builds.
FastCV is loaded dynamically; missing runtime libraries disable only VPP.

Adjust `libdir` if your system uses a driver directory other than `/usr/lib/dri`
(Debian/Ubuntu on ARM64: `-Dlibdir=lib/aarch64-linux-gnu`).
Installation adds the `msm` and `panthor` driver aliases, so **`LIBVA_DRIVER_NAME` is not
required** on the target platforms.

To check the installation (`vainfo` comes from `libva-utils` on Arch):

```sh
vainfo --display drm --device /dev/dri/renderD128
```

## Usage

Play a video with mpv:

```sh
mpv --hwdec=vaapi input.mp4
```

Encode to H.264 with FFmpeg:

```sh
ffmpeg -vaapi_device /dev/dri/renderD128 -i input.mp4 -an \
  -vf format=nv12,hwupload -c:v h264_vaapi -profile:v high \
  -bf 0 -g 60 -rc_mode CBR -b:v 6M -async_depth 4 output.mp4
```

For HEVC, use `-c:v hevc_vaapi -profile:v main` in the command above.

For hardware decoding and encoding of a supported 8-bit input:

```sh
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i input.mp4 -an \
  -c:v h264_vaapi -bf 0 -qp 24 -async_depth 4 output.mp4
```

MP4 and raw H.264 output are supported; for Matroska, encode to raw H.264 first
and remux. FFmpeg may warn about unsupported packed headers because the firmware
produces the headers. Custom VUI/SEI metadata is not forwarded.

Scale a decoded NV12 video on CDSP:

```sh
ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -hwaccel_output_format vaapi -i input.mp4 -an \
  -vf scale_vaapi=w=1920:h=1080 -c:v h264_vaapi -bf 0 -qp 24 \
  -async_depth 4 output.mp4
```

For GStreamer, set `GST_VA_ALL_DRIVERS=1` to enable this driver's `va` elements:

```sh
GST_VA_ALL_DRIVERS=1 gst-launch-1.0 -e \
  videotestsrc num-buffers=120 ! \
  video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1 ! \
  vah264enc rate-control=vbr bitrate=6000 target-percentage=100 \
    b-frames=0 ref-frames=1 target-usage=1 key-int-max=60 ! \
  'video/x-h264,profile=high' ! h264parse ! mp4mux ! filesink location=output.mp4
```

For Sunshine/Moonlight, use **H.264 / SDR**. HEVC and AV1 encoder probes may fail
while H.264 remains available.

## Configuration and development

| Variable | Purpose |
| --- | --- |
| `LIBVA_DRIVER_NAME=v4l2` | Explicitly select this backend. |
| `LIBVA_DRIVERS_PATH=/path/to/build` | Load an uninstalled build. |
| `LIBVA_V4L2_DECODER_DEVICE` / `LIBVA_V4L2_ENCODER_DEVICE` | Override automatic video-device discovery (also `IRIS_VAAPI_DEVICE` / `IRIS_VAAPI_ENCODER_DEVICE`). |
| `IRIS_VAAPI_COPY=gpu` or `cpu` | Select the copy path; default is `gpu`. |
| `IRIS_VAAPI_DEBUG=1` | Enable diagnostics. |

### CIX Sky1 / Orange Pi 6 Plus notes

- Discovery uses the Linlon nodes named `mvxdec` / `mvxenc` and does not open other video
  nodes (the vendor ISP node is known to misbehave when probed).
- Decoding requests decode-order output through the Linlon `frame reordering` control and
  marks each compressed frame as complete, so every VA picture finishes on its own.
- Linlon reports NV12/P010 as two V4L2 planes. Decoded surfaces are exported as two-object
  DRM PRIME descriptors with 128-byte pitches; `vaDeriveImage` maps both planes into one
  range. `vaAcquireBufferHandle` is unavailable for these surfaces.
- Surfaces which are not decoder output are allocated from `/dev/dma_heap/system` when
  accessible, then `/dev/udmabuf`, then a linear GBM buffer on the Panthor render node.
- The GPU copy uses GLES 3.1 (Panfrost) with `EXT_copy_image` or a framebuffer blit.
- Encoding copies NV12 input into the VPU queue on the CPU. Parameter sets, which the
  Linlon firmware emits once as a separate buffer, are merged into the first picture and
  repeated before every forced keyframe. B frames remain unsupported.

Quick check on the board:

```sh
v4l2-ctl --list-devices          # expect mvxdec and mvxenc
vainfo --display drm --device /dev/dri/renderD128
IRIS_VAAPI_DEBUG=1 ffmpeg -hwaccel vaapi -hwaccel_device /dev/dri/renderD128 \
  -i input.mp4 -f null -
```

Use clang-format 22 and the checked-in `.clang-format`. Meson provides `format`
and `format-check` targets when clang-format is available; `-Dwerror=true`
enables compiler warnings as errors. Hardware tests and their run instructions
are in [tests/](tests/). Local research and historical results live in ignored
`tmp/` and are not required to build the driver.

## License

The backend is [MIT licensed](LICENSE). Bundled Linux kernel patches are
GPL-2.0-only; external test frameworks and media retain their own licenses.


# libva-v4l2_request-avd (unofficial fork)

An unofficial fork of the VA-API driver for V4L2 stateless decoders, carrying the Apple Video
Decoder (AVD) support used by Asahi Linux. It lets VA-API applications (mpv, Chromium, FFmpeg)
decode video on the AVD hardware in Apple Silicon Macs.

**This fork is not affiliated with or endorsed by the authors of the projects it is based on.**
Please don't report problems with it to those projects; open an issue here instead.

To set up hardware video decoding on an Omarchy Mac with this driver and the matching kernel
driver patches, use [omarchy-m1-video](https://github.com/iconidentify/omarchy-m1-video).

The original documentation is in [README](README).

## Support contract

Codec, device and client claims are defined in [docs/SUPPORT.md](docs/SUPPORT.md)
and the versioned [support matrix](docs/support-matrix.json). r11 evidence is M1
(T8103) only: HEVC 144/147, AVC 73/135, opt-in High 10 FRExt 27/69 and VP9
216/305, with exact pass sets pinned. Those fractions are raw suite totals, not
eligible-subset percentages. No matrix row is currently `supported`. Compiled-in
AV1/VP8/MPEG-2 backends and 12-bit/4:2:2/4:4:4 formats are untested or
unsupported, not inferred from source files. Safe rejection is not decode support.

A separately qualified local FFmpeg n9.0.1 build with the pending-parameter-set
client patch passes HEVC 145/147 on the same r11 driver, adding
`VPSSPSPPS_A_MainConcept_1` without losing a pass. The support matrix pins the exact
client commit and patch hash. That client is not packaged or installed, so the shipped
r11 baseline remains 144/147 and this is not a driver-only improvement.

## Development roadmap

See the [roadmap and ticket index](docs/ROADMAP.md) for planned codec, reliability,
client/display and release work across both repositories. Agents and contributors should
read the [claim and execution workflow](docs/AGENT_WORKFLOW.md) before taking a ticket.

## Contributing and reporting

**Contributor priorities:** [choose an offline codec or AVD task](docs/CONTRIBUTOR_START.md).
Humans and coding agents can contribute without owning an Apple Silicon test machine.

Start with [CONTRIBUTING.md](CONTRIBUTING.md) for repository ownership, offline checks
and PR evidence. Use [SECURITY.md](SECURITY.md) for suspected vulnerabilities, and
[maintenance and handoff](docs/MAINTENANCE.md) for triage, review and release decisions.

## Upstream and credits

- **libva-v4l2_request** by Ondřej Jirman (megi): the driver this all builds on.
- **[sofus13/libva-v4l2_request](https://github.com/sofus13/libva-v4l2_request)**, tag `1.3`
  (`cfe6c2a`): AVD support. This fork keeps that full history and its tags.
- **Chromium green-frame fix**, from branch `fix-avd-early-export` of
  [Ante042/libva-v4l2_request](https://github.com/Ante042/libva-v4l2_request), kept as the original
  commits:
  - `201bc71` by Igor Ryzhkov: decode into surfaces the client exported before the first decode
  - `9e6d750` and `386956d` by Ante042: keep exported dimensions, reserve codec tail storage, and a test

## Changes carried by this fork

On top of sofus13's tag `1.3`:

- `201bc71`, `9e6d750`, `386956d` (Igor Ryzhkov, Ante042): Chromium creates each VA surface, exports it
  as a dma-buf and imports it into the GPU before decoding into it. Version 1.3 decoded into different
  buffers, so Chromium showed solid green video. With these commits the decoder writes into the
  exported buffers.
- H.264: drop trailing zero bytes from slice data. They made the AVD firmware hang on pictures with
  several CAVLC slices.
- HEVC: fix the slice header parser that finds entry point offsets (VA-API does not carry them); tiled
  and wavefront streams hung on their first P picture.
- P010: size the exported backing of 10-bit surfaces for the decoder's reference data; 10-bit HEVC
  decoded to blank frames.
- Log why an exported surface backing does not fit the CAPTURE format.
- HEVC: reject out-of-range reference counts, exp-Golomb codes and CTB sizes in untrusted slice
  headers; a crafted slice could overflow the stack.

Version `1.3.r6` also:

- Reports failed CAPTURE buffers as `VA_STATUS_ERROR_DECODING_ERROR` and propagates flush,
  request, conversion and buffer-reuse wait failures.
- Fixes FFmpeg `hwdownload` crashing when context destruction unmaps a frame during `vaGetImage`.
  Surface operations and context teardown are serialized; live surfaces keep their frame storage,
  and derived images own their mappings. Pending frames finish before teardown, with errors
  retained on the surviving surfaces.
- Bounds image copies, preserves the last UV pair on odd-width NV12/P010 images, rejects invalid
  image dimensions, and avoids a dangling buffer after a zero-element resize.
- Checks HEVC entry-point capacity and offset lengths, resets offsets between request batches,
  and rejects malformed headers before submission.
- Includes offline regression tests, sanitizer CI, and hardware pixel-comparison scripts.

Version `1.3.r7` adds opt-in H.264 High 10, an explicit FFmpeg quantizer workaround,
10-bit capability checks, H.264 parser regressions, and a conformance runner that requires
hardware frames and preserves resolution changes. See [codec testing](tests/README.md#codec-conformance-without-software-fallback).

Version `1.3.r8` preserves short-term-only HEVC references in decode order on AVD and remaps all slice/RPS
indices. This corrects all 300 pictures of `RPS_B_qualcomm_5`, raising the serial HEVC total
to 144/147. The firmware's ordering sensitivity remains unexplained; `RPS_E_qualcomm_5`
still fails. Streams permitting long-term references and other V4L2 decoders retain the
original reference order: the unrestricted experiment increased RPS_E corruption.

HEVC also rejects invalid active references and malformed slices before flushing a preceding
batch. A failed RenderPicture blocks subsequent submission of the incomplete picture.
Unused unavailable references around random-access points remain allowed.

Version `1.3.r9` rejects incomplete H.264 pictures, overflowing slice-parameter counts,
and invalid active references before submitting a pending slice. Missing surface storage
cannot match another missing reference through timestamp zero. Slice types must also
agree with the parsed NAL header. Three new offline cases reproduce the earlier failures
and verify valid submissions and recovery, bringing the sanitizer suite to 25 cases.

Version `1.3.r10` validates VP9 headers and submission completeness, preserves the colour-range
flag on inter pictures, and commits loop-filter/segmentation/range changes only after successful
submission. Missing reference storage, including references from an older decoder context,
is rejected before hardware submission. Four VP9 cases bring the sanitizer suite to 29 cases,
including 24,000 additional deterministic parser inputs.

Version `1.3.r11` protects the shared picture lifecycle and reference lookup:

- Reserve fresh targets at BeginPicture and reject destruction while a picture still uses them;
  a malformed API sequence previously left EndPicture with a freed target pointer.
- Keep the first RenderPicture failure through EndPicture, prevent nested BeginPicture calls
  from replacing staged work, and clean up after a failed codec begin.
- Preserve a failed submission when an earlier slice finishes; a successful new submission
  resets the status on surface reuse.
- Resolve reference timestamps only for valid buffers owned by the requesting decoder context.
  Missing, detached, mismatched and known-failed references cannot alias another context's buffer.
- Validate context arguments and treat render-target lists as hints without changing ownership.

Six new offline cases bring the sanitizer suite to 35 cases. The lifetime bug is reproduced
under ASan using an intercepted codec; it is an API misuse reproduction, not a demonstrated
malicious-video exploit. Full VP9 inter-frame resize support still needs kernel-side work.

Tested on an M1 (T8103) with the kernel patches from omarchy-m1-video: `JCT-VC-HEVC_V1` 144/147 and
`JVT-AVC_V1` 73/135 bit-exact through FFmpeg VA-API. Earlier Chrome 152 H.264 playback tests
matched software rendering; this revision adds direct early-export pixel regressions.

## Known problems

As of 2026-09-15:

- **Vulkan output in mpv** (`gpu-api=vulkan`) shows a green/pink ghost picture: Mesa's
  Vulkan driver for Apple GPUs ignores the plane offsets of imported frames. Use `gpu-api=opengl`.
- **H.264 profiles:** Constrained Baseline, Main and High are offered by default. High 10 requires
  the opt-in mode below. 4:2:2 uses software. Interlaced H.264 remains unsupported by AVD.
- **VP9:** 8-bit and 10-bit 4:2:0 are tested. Sub-64-pixel dimensions, inter-frame resizing,
  and some scalable-video vectors remain incomplete. Two resize streams that caused firmware
  timeouts with r9 now fail in userspace when their references belong to an older context.
  This is safe rejection, not support for those streams. See [VP9 testing](tests/README.md#vp9-validation).
- **Early export needs DMABUF import.** Once a context decodes into client-exported surfaces, a later
  surface whose layout does not match fails instead of falling back to separate buffers.
- **Kernel driver bugs** in AVD itself can hang or crash the system; the kernel patches in
  [omarchy-m1-video](https://github.com/iconidentify/omarchy-m1-video) fix several of them.

The remaining HEVC mismatches, boot-reset investigation, Chrome colour issue and Firefox
validation are tracked in [omarchy-m1-video's gap status](https://github.com/iconidentify/omarchy-m1-video/blob/main/docs/GAP_STATUS.md).
A successful decode call cannot detect a firmware-produced wrong picture without an error flag;
the conformance failures remain open.

## Building

```sh
meson setup build
meson compile -C build
```

To try the driver without installing it:

```sh
LIBVA_DRIVERS_PATH=$PWD/build/src LIBVA_DRIVER_NAME=v4l2_request mpv --hwdec=vaapi-copy video.mp4
```

### Installing

`meson install` copies only the driver module, `v4l2_request_drv_video.so`, into libva's
driver directory. It does not touch the kernel, install kernel patches, or reboot anything;
hardware/kernel setup on Apple Silicon is a separate step handled by
[omarchy-m1-video](https://github.com/iconidentify/omarchy-m1-video).

```sh
meson setup build --prefix=/usr
meson compile -C build
sudo meson install -C build
```

- **Where it installs**: by default the driver directory comes from libva's own pkg-config
  `driverdir` variable (`pkg-config --variable=driverdir libva`; typically `/usr/lib/dri` or
  an arch-tripled `.../dri`). Override it at `meson setup` time with `-Ddriverdir=<path>`
  when packaging for a nonstandard prefix.
- **Uninstalling**: `sudo ninja -C build uninstall` removes exactly the files `meson install`
  put down, including now-empty parent directories it created; nothing else under the
  prefix is touched.
- **Disposable-root testing**: verify a clean install/uninstall without touching the host
  using `DESTDIR`:

  ```sh
  meson setup build --prefix=/usr
  meson compile -C build
  root=$(mktemp -d)
  DESTDIR="$root" meson install -C build
  DESTDIR="$root" ninja -C build uninstall
  rmdir "$root"   # uninstall preserves this pre-existing DESTDIR directory
  ```

  `tests/install-smoke.sh` automates this for both the default and a custom `-Ddriverdir`,
  and additionally checks that the installed module is stripped, carries no build/worktree
  paths, and matches its libva ABI entrypoint (see [Verifying an
  install](#verifying-an-install) below).
- **Installed artifacts**: only the driver module has `install: true` in `src/meson.build`;
  test binaries and the build directory are never installed. The project defaults to
  `-Dstrip=true`, so the installed copy omits DWARF debug paths. The smoke test also checks for
  embedded source/build paths; stripping alone cannot remove arbitrary strings. Pass `-Dstrip=false` (e.g. for a `-dbg` package) to keep
  symbols. The unstripped build-tree copy under `build/src/` is unaffected, so
  `LIBVA_DRIVERS_PATH=$PWD/build/src` development keeps full debug info.

### Dependency bounds

The libva floor below is enforced at configure time: `meson setup` fails naming
the version it found versus the floor when libva is too old. An
explicitly `-Dcodec_<name>=enabled` codec fails configuration naming the missing kernel
control instead of silently compiling out (see the `codec_checks` table in `meson.build`).

| Dependency | Floor | Verified oldest combination | Notes |
|---|---|---|---|
| libva (`libva-dev`) | VA-API >= 1.7.0 (library release >= 2.7.0) | Ubuntu 20.04, libva 2.7, gcc 9 | `dependency('libva', version: '>= 1.7.0')`. DRM_PRIME_2 surface export needs 2.4; opt-in H.264 High 10 needs 2.18. |
| libdrm (`libdrm-dev`) | any pkg-config `libdrm` | — | Presence only, no version floor enforced. |
| Linux UAPI headers (`linux-libc-dev`) | Per-codec symbol checks: 5.11 (H.264) through 6.5 (AV1); 5.4 is the oldest verified header set, not a global configure-time floor | Ubuntu 20.04, headers 5.4 (all stateless codecs compiled out) | Auto-detected from the installed UAPI *headers*, not the running kernel; each codec's exact floor is documented in `meson.build` and `meson_options.txt`. |
| Meson, Ninja, a C11 compiler | — | GCC 9 and Clang, both exercised in CI | `c_std=gnu11`. |

These floors and the oldest-verified rows are exercised on every PR by the
[`checks.yml`](.github/workflows/checks.yml) CI matrix (its "current dependencies",
"oldest verified dependencies (libva 2.14, kernel UAPI 5.15)" and "oldest kernel UAPI (5.4
headers, all stateless codecs compiled out)" jobs); that workflow is the authoritative
source for pinned container identities and dependency tiers; package versions within
those images are resolved from their archives and recorded in each run, not pinned individually.

### Verifying an install

`LIBVA_DRIVER_NAME=v4l2_request vainfo --display drm` on a qualified decoder should report the vendor string
`v4l2-request (omarchy-m1-video 1.3.r11)` (or the fork's current version). A different or
missing string means `LIBVA_DRIVERS_PATH`/`driverdir` picked up a different module. If
`vainfo` instead fails to find or load the driver, the two most likely causes are:

- **ABI entrypoint mismatch**: libva looks up a version-specific symbol,
  `__vaDriverInit_<VA_MAJOR>_<VA_MINOR>`, generated from the libva headers this module was
  *built* against. Compare that to what the *runtime* libva expects:

  ```sh
  nm -D build/src/v4l2_request_drv_video.so | grep vaDriverInit   # symbol this build exports
  pkg-config --modversion libva                            # development metadata, not runtime proof
  ```

  The [libva 2.24 loader](https://github.com/intel/libva/blob/2.24.0/va/va.c#L380-L404)
  tries its current VA minor and then older minors within the same major. An older
  driver minor can therefore be found by a newer runtime; exact minor equality is
  not required. A driver built for a newer minor than the runtime exports an entrypoint
  that runtime does not probe. Check the VA-API version and loader errors printed by
  the actual client (pkg-config can describe a different development installation),
  then rebuild against headers compatible with the deployed runtime if necessary.
  Symbol lookup alone does not establish working device initialization or decoding.
- **Wrong driver directory**: confirm the installed path matches what libva actually
  searches, `pkg-config --variable=driverdir libva` (or the `-Ddriverdir` used at build
  time), and that `LIBVA_DRIVER_NAME=v4l2_request` is set so libva picks
  `v4l2_request_drv_video.so` there.

### H.264 High 10 with FFmpeg or mpv

High 10 is disabled by default because FFmpeg 9.0.1 passes a bit-depth bias in its VA-API
picture quantizers: a stream with QP 21 arrives as QP 33. The compatibility mode removes
that bias. Both complete FREH10 conformance streams (718 frames) matched the reference
checksums on the M1 with this mode:

```sh
LIBVA_DRIVERS_PATH=$PWD/build/src LIBVA_DRIVER_NAME=v4l2_request \
LIBVA_V4L2_H264_HIGH10=ffmpeg mpv --gpu-api=opengl --hwdec=vaapi-copy high10.mkv
```

`LIBVA_V4L2_H264_HIGH10=native` enables the profile for clients that already send the PPS
syntax values. `ffmpeg` enables it and adjusts the biased quantizers; use it only with
affected FFmpeg-based clients. Unset the variable (or use `off`) to disable High 10.
Both enabled modes also require the decoder to accept a 10-bit SPS and offer usable
10-bit 4:2:0 output. Other hardware and all High 10 coding features are not yet validated.

The offset is visible in FFmpeg's [PPS parser](https://github.com/FFmpeg/FFmpeg/blob/n8.0/libavcodec/h264_ps.c)
and [VA parameter construction](https://github.com/FFmpeg/FFmpeg/blob/n8.0/libavcodec/vaapi_h264.c).
This is a client compatibility setting, not a change to the VA-API or V4L2 parameter contract.

### Diagnostics

Driver messages go to standard error as `libva-v4l2request: ...` lines. Set
`LIBVA_V4L2_DIAG=json` to get one JSON record per line with a stable failure category
(`unsupported`, `client`, `bitstream`, `reference`, `allocation`, `timeout`, `kernel`,
`decoder`, `device`), the failed operation, errno, a per-context identifier and the
driver's detected capabilities. Paths and URLs are redacted and output is rate limited.
See [docs/DIAGNOSTICS.md](docs/DIAGNOSTICS.md) for the schema and what to include in a
report.

## Tests

See [tests/README.md](tests/README.md) for the offline sanitizer suite and guarded hardware
checks. `vainfo --display drm` identifies this build as
`v4l2-request (omarchy-m1-video 1.3.r11)`.

## License

GPL-3.0-or-later, see [COPYING](COPYING).
