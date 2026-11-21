# Testing the AVD fork

## Offline tests

These compile the actual driver sources with a fake V4L2 device. They need Meson, Ninja,
GCC/Clang, libva and libdrm development headers, but no decoder, root access or kernel module.
`early-export-backing` intercepts standalone `VIDIOC_CREATE_BUFS` on coherent
`V4L2_MEMORY_MMAP` (`count = 1`, no cache-hint probe): success, `-ENOMEM` →
`VA_STATUS_ERROR_ALLOCATION_FAILED`, `-EINVAL` → `VA_STATUS_ERROR_OPERATION_FAILED`.
It does not claim hardware frames.

```sh
meson setup build-test -Db_sanitize=address,undefined
meson test -C build-test --print-errorlogs
```

`python3 tests/support-matrix.py` (also the Meson `support-matrix` test) validates
the versioned support contract in `docs/SUPPORT.md` and `docs/support-matrix.json`.
It checks advertised VA profiles, the r11 raw suite totals, pinned pass sets, and
fixtures that reject a `supported` row without provenance or a result artifact.
It does not open a decoder.

Compare a candidate run with the pinned r11 pass sets (no decoder):

```sh
python3 tests/compare-results.py --report docs/r11-pass-sets.json
python3 tests/compare-results.py \
  --baseline docs/r11-pass-sets.json \
  --candidate /path/to/summary.json \
  --suite hevc
```

`compare-results.py` reconstructs HEVC 144/147, AVC 73/135, FRExt 27/69 and VP9
216/305 from the pinned record. It fails if one old pass is lost even when the
fraction is unchanged, and it will not treat software fallback, timeouts, aborts
or malformed JSON as a green hardware result. `conformance.py` writes
`summary.json` in the result directory for this command. The Meson
`conformance-result` test is the schema/comparator self-check.

The cases cover failed decode/export, CAPTURE/reference/GPU-reader/request timeouts,
invalid poll events, deferred flush failures, surviving surfaces and derived images after context destruction,
errors during teardown, grown OUTPUT indices, bitstream size overflow, odd-width NV12/P010
copies, truncated image backing, invalid dimensions and zero-element buffer resizing.
The HEVC case checks exact-capacity entry points, malformed headers and 24,000 deterministic
random inputs, AVD reference ordering and index remapping, retained long-term references,
unavailable references at random-access points, and failed-picture submission. Two
`hevc-capability-*` cases pin the range-extension boundary from
[docs/HEVC_RANGE_EXTENSIONS.md](../docs/HEVC_RANGE_EXTENSIONS.md): only Main and Main10 are
enumerated, every libva RExt/SCC profile and every non-4:2:0 or wrong-depth RT format is
refused at `vaCreateConfig` without leaking a handle, and unequal-depth, equal 9/11-bit, 12-bit,
monochrome, 4:2:2, 4:4:4 and separate-plane pictures are refused at `vaRenderPicture` with no
SPS staged and no ioctl issued, in both AVD and generic contexts, while 8/10-bit 4:2:0 pictures
are staged unchanged. H.264 adds another 24,000 parser inputs, truncated/unsupported NALs, slice-group
rejection, High 10 quantizer modes and fake-device capability checks. Three H.264 submission
cases cover missing slice data at EndPicture, slice-count overflow, invalid/missing active
references, contradictory slice types, preserving a staged slice's controls, and recovery
on the next picture. Submission is intercepted in-process; no device is opened.
Five `diag-*` cases cover the diagnostics in `docs/DIAGNOSTICS.md`: every failure category
(unsupported profile, client call order, oversized bitstream, CAPTURE allocation failure,
request timeout, rejected controls, decoder-flagged frame and invalid device poll) is forced
through the fake device in both text and JSON mode with identical VA status, and the VP9 and
H.264 reference cases check the `reference` category. They also check that reused VA context
IDs keep distinct `ctx` serials, path/URL redaction and size limits, per-category rate
limiting, and report a bounded synthetic logging overhead. `diag-schema` validates emitted
JSON against `tests/fixtures/diagnostics/schema.json`, rejects malformed fixture records,
and fails if the category lists in the code, fixture and documentation differ.
Eight `hevc-reftrace-*` cases cover the opt-in HEVC reference-control trace in
[docs/HEVC_REFTRACE.md](../docs/HEVC_REFTRACE.md) (issue #84): synthetic IDR/P/B
sequences are submitted through the HEVC backend to an in-memory device that captures
every `VIDIOC_S_EXT_CTRLS` payload, once with the trace off and once on; the payloads and
VA statuses must be byte-identical, and every record must match the captured decode and
slice parameters (DPB slots, RPS lists, reference indices) in AVD, generic and
LTR-capable-SPS contexts, including zero-long-term pictures under such an SPS, a real
long-term reference, mid-picture slice-batch flushes, the 16-slice record bound, a rejected
submission (no record) and the environment contract (off by default, no path echoed).
`hevc-reftrace-check` runs the offline differential checker's self-test against the
synthetic fixtures in `tests/fixtures/hevc-reftrace/` and, when HEVC is built, against
the writer's real output. There are
40 sanitizer Meson cases plus the `support-matrix`, `conformance-result`,
`diag-schema`, `hwguard`, `rps-e-research` and HEVC concurrency research checks.
The count-overflow case injects the boundary into codec state rather than
allocating billions of real slices; it is an arithmetic regression, not proof of a practical
malicious-video exploit.
Four VP9 cases cover malformed/incomplete headers, failed-submission state rollback, colour-range
inheritance, missing/cross-context references and 24,000 deterministic parser inputs. These join
the H.264 and HEVC inputs for 72,000 generated inputs across three registered parser cases.
Those generated inputs are smoke coverage, not independent tests. Coverage-guided
no-device fuzzing (issue #22) lives in `fuzz-h264`, `fuzz-hevc`, `fuzz-vp9` and
`fuzz-va-api`: each replays pinned synthetic seeds twice under ASan/UBSan and
compares a normalized oracle (a second differing call is a harness failure).
`fuzz-budgets` checks oversize file-input truncation, non-regular I/O exit 99
and the replay `SIGALRM` handler; `fuzz-timeout-campaign` proves campaign
builds leave `SIGALRM` to libFuzzer; `fuzz-replay-mismatch` proves identity
compare rejects a non-deterministic stub. `fuzz-provenance` checks
`tests/fuzz/provenance.json`. libFuzzer campaign binaries are opt-in
(`-Dfuzzing=enabled`) and are documented in [../docs/FUZZING.md](../docs/FUZZING.md);
the 24 CPU-hour run is not a meson test and must not run on public PR CI.
Clang CI runs `sh tests/fuzz-campaign.sh --smoke`.
Six shared-lifecycle cases add failed Render/Begin recovery, active-target lifetime, reference
ownership, invalid context arguments, and submission errors surviving a later buffer completion.
`picture.c` calls the public picture entrypoints with an intercepted codec. The original target
lifetime failure produces an ASan use-after-free when the active target is destroyed before
EndPicture; this does not establish that a media file can trigger the same API sequence.
CI also runs `frame-check.sh` in software to test resolution changes and truncated input;
hardware tests are separate.

`concurrent-va-worker` is the real FFmpeg client for later VA qualification.
Its software self-test executes 18 groups: 1/2/4 threads or processes, normal,
midpoint teardown and a live-decoder EOF rejection. H.264/HEVC/VP9 8/10-bit
outputs are checked against independent FFmpeg-CLI raw-pixel references,
including every stream's exact count/digest and retained frame across teardown.
The negative suite rejects oracle mutations and exercises actual owned process
launch/exit/deadline/output/cancellation cleanup, escaped stdout, malformed or
short inputs, and the worker's start barrier. Self-tests open no decoder device;
an explicit unguarded-VA refusal check exits before device initialization.

Threads share one device and preserve other live decoders during victim
teardown; their API calls are paused until that teardown completes. This is not
proof of real in-driver overlap, derived-image/export lifetime or VA error
recovery. Existing model overlap/TSan tests remain unchanged. Explicit hardware
mode requires the portable guard and rejects software frames. See the finite
[hardware runbook](../docs/CONCURRENT_VA_HARDWARE.md); hardware acceptance and
remaining lifetime criteria stay in #36.

Three resource-churn cases run 1,000 normal, early-export and held-derived-image
lifecycles in one initialized fake driver. At cycles 100, 500 and 1,000, and
after every intervening cycle, model descriptors, mappings, heap allocations and
live VA objects must return exactly to the initialized-driver baseline. The
operation-index failure sweeps now prove the same in-process cleanup before final
`vaTerminate`, then complete a fresh decode and return to baseline again. Handle
table capacity is recorded separately as a bounded high-water cache.

`python3 tests/resource-churn.py self-test` validates the process-local `/proc`
sampler used for later hardware campaigns. It records fsynced JSONL for FD,
mapping, RSS and available dma-buf fdinfo measurements, requires an explicit RSS
threshold in acceptance mode, and never treats unavailable dma-buf accounting as
zero. See [the resource churn contract](../docs/RESOURCE_CHURN.md).

CI also runs `sh tests/shared-contexts.sh` in software. With a driver-directory argument,
run it through the hardware guard to interleave H.264, HEVC and 8/10-bit VP9 decoders on
one shared VA display. The clips contain 24, 36, 48 and 60 frames, so earlier contexts are
destroyed while later ones continue. Each stream must match its independently decoded
software checksum. Normal and early-export runs compare 336 hardware output frames.
This interleaves work in one thread; it does not measure concurrent API calls or throughput.

The `concurrent-stress` Meson cases (issue #36) add offline caller schedules: they call the
real public entrypoints from multiple threads against an in-memory model decoder
(no device — the model completes queued work after a seeded number
of model-time ticks, with short real sleeps pacing the held overlap window). `threads-1/2/4` decode mixed-codec
frames, read them back through GetImage, exported dma-bufs and derived images, and
destroy each context while later streams continue; `teardown-1/2/4` destroy one
context mid-decode and require every published frame to complete byte-exact while
the other streams verify all of theirs; `failure-4` has a separate misbehaving client
(bogus ids, foreign surfaces, double destroys, live context churn) that owns its own
context while every valid stream completes exactly. **In-driver overlap is measured
inside the driver itself** (`v4l2r_overlap_*` in `src/api.c` with brackets in the
unlocked entrypoints): every repetition of every schedule asserts the api_mutex
serialization invariant (at most one active locked section), and the
`overlap-2`/`overlap-actor-2` cases make the locked+unlocked overlap deterministic
— the model device holds a chosen request so a `vaSyncSurface` spins inside the
driver holding api_mutex, and the required unlocked entrypoint executions are
recorded against it, fired by a valid decoder thread (`overlap-2`) or by the
failure actor's own unlocked operations (`overlap-actor-2`). The selected sync
waiter must be observed inside the driver before a thread-local delta counts eight
outer calls from the chosen worker. Nested helpers and same-thread calls are
excluded. `overlap-late-2` forces a waiter to arrive after the latched release;
`overlap-counters` checks counter ownership and nesting. The counters are
printed per repetition as `driver_overlap …`. The model device is the decode
oracle: slice bytes are hashed when a request is queued and the completion writes a
pattern derived from that hash into the target CAPTURE plane, so lost frames,
cross-stream pixels and stale buffer reuse all fail an exact byte comparison. The
decoder only reuses a surface after the reader verified its previous frame, matching
a real decoder surface pool. Per-stream MD5s, seeds, ioctl/poll/completion counts are
printed for the evidence record; the mid-decode teardown victim is destroyed while
it provably holds a staged picture open (between BeginPicture and its buffer
creation) and verifies exactly half its frames byte-exact — every other count and
hash is exact, and a first-call rendezvous coordinates caller starts outside the
driver.
`concurrent-process.py` drives the same single-stream worker in 1/2/4
separate processes behind a start barrier (per-process isolation; every worker's
digest is derived independently from the declared seed/frame recipe and compared
exactly, negative fixtures cover stalled workers, inherited stdout holders and
launch failures; one real decoder is the separate guarded hardware gate), and
`concurrent-tsan.sh` re-runs the schedules under ThreadSanitizer in a fresh
sanitizer build with the same compiler Meson uses, skipping with exit 77 and the
printed reason only for known runtime-unavailable startup failures. Schedules,
seeds and recorded hashes: [../docs/CONCURRENCY_STRESS.md](../docs/CONCURRENCY_STRESS.md).

CI also runs `sh tests/install-smoke.sh` (issue #34): it builds and installs the driver
twice into disposable `DESTDIR` roots — once at the default, libva pkg-config-derived
driverdir, once with a custom `-Ddriverdir` — and checks that exactly one file (the driver
module, no test binaries) is installed, that it carries no absolute build/worktree path and
no DWARF debug info (the project's `-Dstrip=true` default), that its vendor/version marker
string and libva ABI entrypoint symbol (`__vaDriverInit_<major>_<minor>`) are present and
match the libva it was configured against, and that `ninja uninstall` removes every
installed file and the now-empty directories it created, leaving the disposable root gone.
It never touches the host's actual driver directory.

## Regression corpus

Inputs behind the codec evidence are pinned, licensed and classified in
[`corpus/manifest.json`](corpus/manifest.json); acquisition and provenance are documented in
[../docs/CORPUS.md](../docs/CORPUS.md). No third-party media is redistributed: suites are
downloaded from their distributor on demand, verified against the upstream checksum from the
pinned Fluster suite definition, and cached in Fluster's
`<suite_name>/<vector_name>/<input_file>` layout so the cache can be passed to
the positional `resources` argument of `conformance.py`.

```sh
python3 tests/corpus.py self-test                                            # offline, no decoder
python3 tests/corpus.py validate --fluster /path/to/fluster                   # pins, licences, classifications
python3 tests/corpus.py fetch  --fluster /path/to/fluster --smoke             # bounded subset, on demand
python3 tests/corpus.py verify --fluster /path/to/fluster --require smoke     # offline, needs no network
```

A suite-wide selection enumerates the **pinned suite definition**, not the manifest's pinned
assets: `fetch --suite ID` takes every vector of that suite, `--all` takes all 662 (and refuses
without `--confirm-large-corpus`), and `--dry-run` prints the plan while acquiring nothing. The
same choice appears in `verify`: `--require smoke` checks the smoke subset, `--require all`
checks the whole corpus. Pinning a vector's SHA-256 is a separate, reviewed step — `fetch` writes
the identities it acquires to `<cache>/corpus-lock.json`, `verify` checks anything in that lock,
and `corpus.py lock` reports hashes for review instead of editing the manifest.

Licence decisions are recorded with the evidence behind them: each entry states whether usable
terms were `identified` or `not-established`, where they were checked and what that implies. For
the official vectors the honest answer is `not-established` (the ITU notice grants no
reproduction right; the WebM test-data directory has no licence file), so nothing is
redistributed and the vectors stay download-on-demand.

The manifest also records **why** each r11 failure is expected
(`unimplemented-syntax`, `requires-profile-override`, `unsupported-hardware-format`,
`unsupported-profile`, `unsupported-dimension`, `expected-rejection`, `known-wrong-output`,
`capability-boundary`). `validate` re-derives every classification from the pinned suite and
fails if any r11 failing vector is undocumented, if a classification drifts, or if a vector the
r11 record passes is called a failure. Corrupt fixtures are marked as such and are never
reference output.

## CI matrix, codec options and the aggregate required check

`.github/workflows/checks.yml` runs on GitHub-hosted runners only and never opens a
decoder. The `userspace` job is the stable aggregate required check: it evaluates the
`needs` context of every child job through `tests/ci_check.py check` and passes only when
each required child job actually ran and succeeded. A failing, cancelled, skipped or
missing child can never leave the aggregate green, and the aggregate itself evaluates
after dependency failures. `tests/ci_check.py fixtures` (Meson test
`ci-aggregate-fixtures`) reproduces each of those failure states offline, and
`tests/ci_check.py audit --workflow .github/workflows/checks.yml` (Meson test
`ci-workflow-audit`) revalidates the actual wiring: every child job must appear in the
aggregate's `needs`, external actions and container images must be pinned by full commit
SHA or sha256 digest, only hosted runner labels are permitted, matrix jobs must not be
fail-fast, and the workflow may only hold `permissions: contents: read`. A full-SHA pin
alone does not say which runtime a pinned action's own `action.yml` targets, which is how
both `checkout` and `cache` quietly drifted onto a deprecated runtime (issue #63): every
pinned commit is also checked against `ACTION_RUNTIME_ALLOWLIST` in `ci_check.py`, a
reviewed, offline `commit -> {runtime, reviewed}` record built by reading each commit's
`action.yml` directly (no network access from this script). A pin missing from that
allowlist, or one whose recorded runtime has fallen out of `SUPPORTED_RUNTIMES`, fails the
audit; add a dated entry when introducing or moving a pin.

Codec build options `-Dcodec_h264|hevc|mpeg2|vp8|vp9|av1=auto|enabled|disabled`
(auto-detect by default) control which codecs compile in. A codec enabled explicitly
without its kernel UAPI fails configuration with the missing control, the first mainline
kernel providing it (h264 5.11, vp8 5.13, mpeg2 5.14, vp9 5.17, hevc 6.0, av1 6.5) and
the remediation; auto-detection compiles the codec out instead. The `image-bounds` test
needs the P010 capture format (Linux 6.0 UAPI) independent of the codec options, and the
HEVC `num_delta_pocs_of_ref_rps_idx` member probe tracks its Linux 6.5 addition. The
multi-slice `V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF` API pair (Linux 5.9) predates every
supported codec pixelformat, so its decode-path uses are compile-time guarded and
unreachable on headers that lack it rather than degraded.
libva's pkg-config Version is the VA API version (libva 2.20 reports 1.20.0), so the
`libva >= 1.7.0` build floor means library release 2.7 — the oldest combination verified
to build and pass the offline suite (Ubuntu 20.04: libva 2.7, gcc 9, 5.4 headers).

Executed CI configurations and their expected Meson test sets (codec-gated tests
register only when the codec is compiled in). The counts are re-measured from the
registered set (enumerate the selected build rather than relying on historical
counts) — they include the r11 regression suite, the
lifecycle, failure-cleanup, diagnostics, corpus and CI checks, and the
concurrent-stress schedules, process checks, counter oracle and ThreadSanitizer
check; historical counts predate several additions:

| Configuration | Codecs | Expected tests |
| --- | --- | --- |
| ubuntu-24.04 and ubuntu-24.04-arm, GCC/Clang, 6.8 UAPI (`build-test`, `codec-options`, `static-analysis`) | all six | full registered suite; enumerate with `meson test -C build --list` |
| ubuntu:22.04 container, 5.15 UAPI (`deps-oldest`, `configure-reject`) | h264, mpeg2, vp8 | HEVC/VP9/AV1-specific cases absent |
| ubuntu:20.04 container, 5.4 UAPI (`uapi-minimal`) | none | codec-gated cases absent; core and Python checks remain |
| all codecs disabled (any headers) | none | core cases, available image cases, and no-device VA fuzz replay |
| `-Dcodec_hevc=enabled -Dcodec_vp9=disabled` | hevc (forced), others auto | compiled-in codec tests, excluding VP9 cases |

`deps-oldest`, `uapi-minimal`, `configure-reject` and `codec-options` assert the
auto-detected and forced test sets in-job. The software `frame-check.sh` runs in all
three dependency tiers, keeping its FFmpeg-facing helpers compatible with FFmpeg 6.1,
4.4 and 4.2; `shared-contexts.sh` runs on the 6.1 and 4.4 tiers only, because its
10-bit VP9 test vector needs the libvpx-vp9 encoder wrapper from FFmpeg >= 4.3.
`static-analysis` builds with
`-Dwerror=true` under both compilers and runs bounded cppcheck at warning level over
the driver target's translation units (the compile database is filtered to them,
since the assert-driven offline suite calls `assert()` with side effects by design
under `-UNDEBUG`); the style-level suggestions that predate this matrix across the
parser suite (const-parameter, shadowed locals) are left for a dedicated cleanup
instead of blanket category disables, and findings that do appear are triaged into
narrow inline suppressions or fixes, never blanket disables.

## Hardware pixel comparisons

Use a normal, unsanitized build, close all video clients, and check that the decoder is idle
and the kernel has no existing decoder faults first. The scripts load the selected userspace
library through `LIBVA_DRIVERS_PATH`; they do not install it or reload the kernel module.
Run them through `python3 tests/hwguard.py`, which holds an exclusive OS lock for
the decoder identity, does a read-only idle/fault preflight, monitors new AVD
journal errors and foreign clients, and uses a finite child deadline. Stop after
a wedge; do not repeatedly open a stuck decoder. A userspace timeout cannot
recover a wedged kernel. The guard never unloads modules. Hardware scripts refuse
to run without `LIBVA_HW_GUARD_LEASE` from this wrapper. Publishable logs redact
paths and URLs; `--verbose-log` is a local opt-in.

```sh
python3 tests/hwguard.py --self-test
meson setup build
meson compile -C build
python3 tests/hwguard.py --deadline 180 -- sh tests/hwdownload.sh "$PWD/build/src" /path/to/main10.bit
python3 tests/hwguard.py --deadline 180 -- sh tests/early-export.sh "$PWD/build/src"
python3 tests/hwguard.py --deadline 180 -- sh tests/h264-high10.sh "$PWD/build/src"
```

`hwdownload.sh` creates short H.264 640x360/1920x1080 and HEVC 640x360 clips and compares the
first 30 decoded frames with software, both normally and with `vaDeriveImage` disabled to
force every frame through `vaGetImage`. NV12 and P010 must match byte for byte. The optional
second argument supplies a 10-bit HEVC clip with at least 30 frames when x265 only supports
8-bit encoding; otherwise the script generates one. The test rejects pixel-format fallback.
On the test M1, use `WPP_C_ericsson_MAIN10_2.bit` from the JCT-VC HEVC conformance suite.

`early-export.sh` needs FFmpeg development headers and its `hw_decode.c` example (optional
second argument gives its path). It compares ordinary decode with export-before-first-decode
for five H.264/HEVC/VP9 clips. It also needs the libvpx-vp9 encoder. The preload hook checks
the exported dma-buf pixels after each frame. AVD on the test M1 advertises VP9 profiles 0
and 2; this particular smoke test covers profile 0. The separate VP9 checks below cover
10-bit output and conformance vectors.

`h264-high10.sh` generates six 10-bit H.264 clips: CABAC/CAVLC, QP 1/21/51, four slices,
B pictures and a cropped 640x360 output. It checks each of 12 frames against software,
then repeats with export-before-decode and verifies stable dma-buf identity/layout.
It requires a 10-bit-capable libx264 build and enables the explicit FFmpeg compatibility
mode for this process. The checksum helper requires hardware frames in both modes.

## Full conformance

Wrap in-tree runners with the portable guard. Fluster suites still come from a
separate checkout:

```sh
python3 tests/hwguard.py --deadline 180 -- \
  python3 tests/conformance.py /path/to/fluster/test_suites/h.265/JCT-VC-HEVC_V1.json \
  /path/to/fluster/resources --driver /path/to/build/src --output /path/to/new-results
```

Record the driver commit, kernel package, installed patch digest, loaded-module provenance,
commands, pass/fail vector names and kernel log for each run. `modinfo` identifies the module
on disk selected for the next load, not necessarily the currently loaded binary. Kernel
changes and reboot tests require the consent and recovery procedure in omarchy-m1-video.

## Limits

Offline tests do not validate firmware, DMA coherence, display import or boot stability.
The per-display API mutex prevents teardown racing surface operations; it may serialize work
from separate contexts within one application. Independent processes remain concurrent.
Rockchip conversion/VPP, AV1 and other hardware are not validated by the M1 runs.

## Codec conformance without software fallback

`conformance.py` builds `frame-check.c` using the installed FFmpeg development libraries,
then reads an existing Fluster suite and downloaded resources. It requires actual VA-API
frames for a hardware pass, hashes each frame at its native resolution, and saves frame
checksums, logs and fsynced JSON results. Failed decode calls and corrupt frames fail the
test; a timeout stops the run. It needs `cc`, `pkg-config`, Python 3, libavformat, libavcodec,
libavutil and libswscale development files. Choose a new output directory for each run.

Run hardware commands below through the guard described above. For software, omit `--driver`.

```sh
LIBVA_V4L2_H264_HIGH10=ffmpeg python3 tests/conformance.py \
  /path/to/fluster/test_suites/h.264/JVT-FR-EXT.json /path/to/fluster/resources \
  --driver "$PWD/build/src" --output /path/to/new-results
```

For the five progressive Baseline/Extended streams rejected by FFmpeg's profile selection,
an explicit override decoded bit-exact on the M1:

```sh
python3 tests/conformance.py /path/to/fluster/test_suites/h.264/JVT-AVC_V1.json \
  /path/to/fluster/resources --driver "$PWD/build/src" --output /path/to/new-results \
  --profile-mismatch --vectors BA3_SVA_C MR2_TANDBERG_E MR3_TANDBERG_B \
  MR4_TANDBERG_C MR5_TANDBERG_C
```

The equivalent FFmpeg option is `-hwaccel_flags allow_profile_mismatch`. This is a per-file
workaround for those tested coding features, not full Baseline/Extended support. Interlacing,
FMO, data partitions and other unimplemented syntax still need software or further work.

Do not count a successful FFmpeg process as hardware evidence: the Fluster FFmpeg VA-API
decoder can silently use software for H.264 4:2:2. Its FRExt totals therefore require the
strict frame check above. Also, `VPSSPSPPS_A_MainConcept_1` loses pictures in FFmpeg's parameter-set
parser; preserving native sizes alone does not fix it. Direct GStreamer V4L2 passes that vector.

To test the checksum helper independently, run `sh tests/frame-check.sh`. The optional driver
argument is a guarded hardware comparison: `python3 tests/hwguard.py -- sh tests/frame-check.sh BUILD/src`.

## VP9 validation

Run `python3 tests/hwguard.py -- sh tests/vp9-matrix.sh /path/to/build/src`. It needs a
high-bit-depth-capable libvpx-vp9 encoder and the FFmpeg development libraries. Eight generated
640x360 clips cover 8/10-bit 4:2:0, limited/full range and lossy/lossless encoding, with tile
settings, alternate-reference encoding enabled and two keyframe intervals. Each 24-frame clip
is decoded normally and with early export: 384 hardware output-frame comparisons. The script
verifies encoded pixel format and range, requires actual hardware frames, checks early-export
backing and compares every output pixel with software. This is not a browser display test.

The official WebM vectors can be run with `conformance.py` using Fluster's
`test_suites/vp9/VP9-TEST-VECTORS.json` and `VP9-TEST-VECTORS-HIGH.json`. On the M1, r9 passed
216/305 in the first suite and the single 10-bit 4:2:0 vector (10 frames) in the second.
The high-bit-depth suite also contains five 12-bit or 4:2:2/4:4:4 vectors outside the tested
hardware formats; they are not included in that 1/1 result.

The 89 baseline failures comprise 60 sub-64-dimension streams, two unsupported profile-1
streams, two resize streams that triggered firmware timeouts, 24 inter-frame-resize checksum
mismatches and one scalable-video checksum mismatch. Software passes 88 of these 89; the
scalable-video stream also misses its reference checksum in software, with a different digest.
The candidate retains all 216 baseline passes and passes the 384-frame generated matrix.
With r10's reference checks, both timeout-producing resize streams are rejected in userspace
without new kernel messages. Their decoding support remains open. The installer repository's
[codec status](https://github.com/iconidentify/omarchy-m1-video/blob/main/docs/CODEC_STATUS.md)
records release-package results and exact vector lists.


`conformance-runner` exercises the actual runner with an intercepted frame-check
process: downloaded `yuv420p` hashes are valid hardware results when the strict
VAAPI helper succeeds, explicit fallback stays a failure, and an invalid summary
makes the runner fail. The printed pixel format describes the hashed output,
not the decoder. The hardware guard retains process-group ownership from its fd
snapshot so reaped short-vector children are not mistaken for foreign clients;
live foreign and unknown holders still abort.
## Resource lifecycle qualification

`resource-campaign.py` drives a single long-lived decoder process with quiescent
in-process resource snapshots, software reference hashes, seek/drain, export and
held-image/context teardown checks. See [the campaign contract](../docs/RESOURCE_CHURN.md#synchronized-real-device-campaign)
for the 1,000-cycle and 60-minute guarded schedules, fixed bounds and evidence
limitations. `python3 tests/resource-campaign-check.py` exercises its software
and negative/interrupt paths without a device. Hardware modes require hwguard.
