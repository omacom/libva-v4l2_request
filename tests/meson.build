failure_cleanup = executable('failure-cleanup', 'failure-cleanup.c', driver_sources,
  c_args: ['-UNDEBUG'],
  include_directories: [root_inc, include_directories('../src')],
  dependencies: [libva_dep, libdrm_dep, threads_dep],
  link_args: ['-Wl,--wrap=ioctl', '-Wl,--wrap=poll', '-Wl,--wrap=__poll_chk',
              '-Wl,--wrap=open', '-Wl,--wrap=close', '-Wl,--wrap=dup',
              '-Wl,--wrap=fcntl', '-Wl,--wrap=mmap', '-Wl,--wrap=munmap',
              '-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=realloc', '-Wl,--wrap=free',
              '-Wl,--wrap=open64', '-Wl,--wrap=__open_2', '-Wl,--wrap=__open64_2',
              '-Wl,--wrap=fcntl64', '-Wl,--wrap=mmap64', '-Wl,--wrap=clock_gettime'])
dimensions = executable('dimensions', 'dimensions.c', driver_sources,
  c_args: ['-UNDEBUG'],
  include_directories: [root_inc, include_directories('../src')],
  dependencies: [libva_dep, libdrm_dep, threads_dep],
  link_args: ['-Wl,--wrap=ioctl', '-Wl,--wrap=poll', '-Wl,--wrap=__poll_chk',
              '-Wl,--wrap=open', '-Wl,--wrap=close', '-Wl,--wrap=dup',
              '-Wl,--wrap=fcntl', '-Wl,--wrap=mmap', '-Wl,--wrap=munmap',
              '-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=realloc', '-Wl,--wrap=free',
              '-Wl,--wrap=open64', '-Wl,--wrap=__open_2', '-Wl,--wrap=__open64_2',
              '-Wl,--wrap=fcntl64', '-Wl,--wrap=mmap64', '-Wl,--wrap=clock_gettime'])
foreach case : ['avd', 'avd-missing', 'generic', 'avd-other-codec', 'stepwise', 'discrete', 'missing', 'invalid', 'multiple', 'query-destroy']
  test('dimensions-' + case, dimensions, args: [case], timeout: 15)
endforeach
if conf.get('HAVE_V4L2_CTRL_VP9') == 1
  foreach case : ['picture', 'picture-narrow', 'picture-stepwise', 'picture-discrete',
                  'picture-narrow-generic', 'picture-stepwise-generic', 'picture-discrete-generic', 'picture-selected']
    test('dimensions-' + case, dimensions, args: [case], timeout: 15)
  endforeach
endif
foreach case : ['control', 'capture-queue', 'request-queue', 'dequeue-error', 'poll-interrupted',
                'converter-error-flag', 'converter-source-error', 'converter-export-error',
                'sweep-create', 'sweep-create-single', 'sweep-decode', 'sweep-decode-early',
                'sweep-export', 'sweep-export-single', 'sweep-view', 'sweep-view-dmabuf',
                'sweep-grow', 'sweep-preserve', 'sweep-convert-setup', 'sweep-convert-kick',
                'sweep-convert-wait', 'sweep-vpp', 'sweep-create-vpp', 'sweep-create-grown',
                'sweep-decode-convert', 'dequeue-eintr', 'dequeue-eagain',
                'dequeue-timeout', 'dequeue-stale-ready', 'sweep-view-pair',
                'sweep-export-pair', 'sweep-attach-pair', 'sweep-preserve-pair',
                'dequeue-index', 'converter-index', 'converter-source-index',
                'converter-stale-ready', 'converter-eintr', 'converter-eagain',
                'converter-source-eagain', 'sweep-export-probes', 'sweep-vpp-stride',
                'rollback-error', 'poll-interrupt-deadline']
  test('failure-' + case, failure_cleanup, args: [case], timeout: 15)
endforeach
foreach case : ['normal', 'early-export', 'held-image']
  test('failure-churn-' + case, failure_cleanup, args: ['churn-' + case], timeout: 120)
endforeach
if conf.get('HAVE_V4L2_M2M_HOLD_CAPTURE_BUF') == 1
  foreach case : ['client', 'controls', 'output', 'request']
    test('failure-partial-' + case, failure_cleanup, args: ['partial-' + case], timeout: 15)
  endforeach
endif

regression = executable('regression', 'regression.c', driver_sources,
  c_args: ['-UNDEBUG'],
  include_directories: [root_inc, include_directories('../src')],
  dependencies: [libva_dep, libdrm_dep, threads_dep],
  link_args: ['-Wl,--wrap=ioctl', '-Wl,--wrap=poll', '-Wl,--wrap=__poll_chk'])
regression_cases = ['decode-error', 'submission-error', 'export-error', 'reuse-timeout', 'reader-timeout',
                    'refwait-timeout', 'export-timeout', 'flush-error',
                    'image-dimensions', 'odd-image-copy', 'buffer-zero',
                    'context-lifetime', 'context-error', 'context-timeout',
                    'output-grown', 'request-timeout',
                    'bitstream-overflow', 'derive-bounds', 'invalid-poll']
# image-bounds needs the P010 capture format from Linux 6.0 UAPI headers.
if conf.get('HAVE_V4L2_PIX_FMT_P010') == 1
  regression_cases += 'image-bounds'
endif
foreach case : regression_cases
  test(case, regression, args: [case], timeout: 10)
endforeach

diagnostics = executable('diagnostics', 'diagnostics.c', driver_sources,
  c_args: ['-UNDEBUG'],
  include_directories: [root_inc, include_directories('../src')],
  dependencies: [libva_dep, libdrm_dep, threads_dep],
  link_args: ['-Wl,--wrap=ioctl', '-Wl,--wrap=poll', '-Wl,--wrap=__poll_chk'])
foreach case : ['categories', 'context-identity', 'redaction', 'rate-limit', 'overhead']
  test('diag-' + case, diagnostics, args: [case], timeout: 30)
endforeach

if conf.get('HAVE_V4L2_CTRL_HEVC') == 1
  parser = executable('hevc-parser', 'hevc-parser.c', driver_sources,
    c_args: ['-UNDEBUG'],
    include_directories: [root_inc, include_directories('../src')],
    dependencies: [libva_dep, libdrm_dep, threads_dep])
  test('hevc-parser', parser, timeout: 30)
  # Profile, RT-format and picture-format boundaries from
  # docs/HEVC_RANGE_EXTENSIONS.md; the wrapped ioctl proves no control reaches
  # a device for a refused picture.
  hevc_capability = executable('hevc-capability', 'hevc-capability.c', driver_sources,
    c_args: ['-UNDEBUG'],
    include_directories: [root_inc, include_directories('../src')],
    dependencies: [libva_dep, libdrm_dep, threads_dep],
    link_args: ['-Wl,--wrap=ioctl'])
  foreach case : ['profiles', 'picture']
    test('hevc-capability-' + case, hevc_capability, args: [case], timeout: 30)
  endforeach
  # Opt-in reference-control trace (issue #84, docs/HEVC_REFTRACE.md): every
  # record must match the controls the in-memory device captured, tracing
  # must not change the submitted bytes or the VA status, and the checker
  # validates the writer's real output against its own fixtures.
  hevc_reftrace = executable('hevc-reftrace', 'hevc-reftrace.c', driver_sources,
    c_args: ['-UNDEBUG'],
    include_directories: [root_inc, include_directories('../src')],
    dependencies: [libva_dep, libdrm_dep, threads_dep],
    link_args: ['-Wl,--wrap=ioctl', '-Wl,--wrap=poll', '-Wl,--wrap=__poll_chk'])
  foreach case : ['match-avd', 'match-ltr-sps', 'match-generic', 'long-term', 'slices',
                  'many-slices', 'failure', 'environment', 'fifo', 'write-failure']
    test('hevc-reftrace-' + case, hevc_reftrace, args: [case], timeout: 30)
  endforeach
endif

if conf.get('HAVE_V4L2_CTRL_VP9') == 1
  vp9 = executable('vp9', 'vp9.c', driver_sources,
    c_args: ['-UNDEBUG'],
    include_directories: [root_inc, include_directories('../src')],
    dependencies: [libva_dep, libdrm_dep, threads_dep])
  foreach case : ['malformed', 'state', 'parser', 'references', 'dimensions']
    test('vp9-' + case, vp9, args: [case], timeout: 30)
  endforeach
endif

if conf.get('HAVE_V4L2_CTRL_H264') == 1
  submission = executable('h264-submission', 'h264-submission.c', driver_sources,
    c_args: ['-UNDEBUG'],
    include_directories: [root_inc, include_directories('../src')],
    dependencies: [libva_dep, libdrm_dep, threads_dep])
  foreach case : ['incomplete', 'slice-count', 'references',
                  'unsupported-partition', 'unsupported-extension', 'unsupported-fmo']
    test('h264-' + case, submission, args: [case], timeout: 10)
  endforeach
  foreach case : ['h264-parser', 'h264-profile']
    check = executable(case, case + '.c', driver_sources,
      c_args: ['-UNDEBUG'],
      include_directories: [root_inc, include_directories('../src')],
      dependencies: [libva_dep, libdrm_dep, threads_dep],
      link_args: case == 'h264-profile' ? ['-Wl,--wrap=ioctl'] : [])
    test(case, check, timeout: 30)
  endforeach
endif

api_lifecycle = executable('api-lifecycle', 'api-lifecycle.c', driver_sources,
  c_args: ['-UNDEBUG'],
  include_directories: [root_inc, include_directories('../src')],
  dependencies: [libva_dep, libdrm_dep, threads_dep])
foreach case : ['surface-duplicates', 'surface-parameters', 'surface-null-output',
                'surface-query-null', 'surface-export-null', 'surface-attributes-null',
                'config-parameters', 'config-null-output', 'config-query-null',
                'buffer-null-output', 'buffer-null-map', 'buffer-context',
                'buffer-owner', 'buffer-recycled-context', 'image-buffer-lifetime',
                'image-derived-null', 'image-query-null', 'context-abort-reuse',
                'context-isolation', 'surface-active-access', 'image-surface-lifetime',
                'vpp-source-lifetime']
  test('api-' + case, api_lifecycle, args: [case], timeout: 10)
endforeach

concurrent_stress = executable('concurrent-stress', 'concurrent-stress.c', driver_sources,
  c_args: ['-UNDEBUG'],
  include_directories: [root_inc, include_directories('../src')],
  dependencies: [libva_dep, libdrm_dep, threads_dep],
  link_args: ['-Wl,--wrap=ioctl', '-Wl,--wrap=poll', '-Wl,--wrap=__poll_chk',
              '-Wl,--wrap=open', '-Wl,--wrap=close', '-Wl,--wrap=dup',
              '-Wl,--wrap=fcntl', '-Wl,--wrap=mmap', '-Wl,--wrap=munmap',
              '-Wl,--wrap=open64', '-Wl,--wrap=__open_2', '-Wl,--wrap=__open64_2',
              '-Wl,--wrap=fcntl64', '-Wl,--wrap=mmap64', '-Wl,--wrap=clock_gettime'])
# Threaded schedules: 1/2/4 contexts decoding, reading back and tearing
# down concurrently against the model decoder. Ten repetitions each.
foreach case : ['1', '2', '4']
  test('concurrent-threads-' + case, concurrent_stress,
       args: ['threads', case, '12', '10', '549203187'], timeout: 120)
  test('concurrent-teardown-' + case, concurrent_stress,
       args: ['teardown', case, '12', '10', '812734691'], timeout: 120)
endforeach
test('concurrent-failure-4', concurrent_stress,
     args: ['failure', '4', '12', '10', '3372110043'], timeout: 120)
# In-driver overlap gates (issue #36 AC2): stream 0's frame 0 is held by
# the model device so a vaSyncSurface spins inside the driver holding
# api_mutex, and the required unlocked entrypoint executions are
# recorded by the driver-side instrumentation (v4l2r_overlap_*) — fired
# by the decoder here, by the failure actor in the actor variant.
test('concurrent-overlap-2', concurrent_stress,
     args: ['overlap', '2', '6', '10', '73204115'], timeout: 120)
test('concurrent-overlap-actor-2', concurrent_stress,
     args: ['overlap-actor', '2', '6', '10', '1946285037'], timeout: 120)
test('concurrent-overlap-late-2', concurrent_stress,
     args: ['overlap-late', '2', '6', '10', '73204115'], timeout: 120)
test('concurrent-overlap-counters', concurrent_stress,
     args: ['overlap-counters'], timeout: 30)

picture = executable('picture', 'picture.c', driver_sources,
  c_args: ['-UNDEBUG'],
  include_directories: [root_inc, include_directories('../src')],
  dependencies: [libva_dep, libdrm_dep, threads_dep])
foreach case : ['render-errors', 'begin-errors', 'target-lifetime', 'reference-owner', 'context-parameters']
  test('picture-' + case, picture, args: [case], timeout: 10)
endforeach

fuzz_inc = [root_inc, include_directories('../src')]
fuzz_va_wrap = ['-Wl,--wrap=ioctl', '-Wl,--wrap=poll', '-Wl,--wrap=__poll_chk',
                '-Wl,--wrap=open', '-Wl,--wrap=open64', '-Wl,--wrap=__open_2', '-Wl,--wrap=__open64_2',
                '-Wl,--wrap=malloc', '-Wl,--wrap=calloc', '-Wl,--wrap=realloc', '-Wl,--wrap=free']
fuzz_seed_root = meson.current_source_dir() / 'fuzz' / 'seeds'
fuzz_replay_c = ['-DFUZZ_REPLAY_BUILD']
fuzz_campaign_def = ['-DFUZZ_CAMPAIGN_BUILD']

if conf.get('HAVE_V4L2_CTRL_H264') == 1
  fuzz_h264_replay = executable('fuzz-h264-replay', 'fuzz-h264.c', 'fuzz-replay.c', 'fuzz-oracle.c', driver_sources,
    include_directories: fuzz_inc,
    c_args: fuzz_replay_c,
    dependencies: [libva_dep, libdrm_dep, threads_dep])
  test('fuzz-h264', fuzz_h264_replay, args: [fuzz_seed_root / 'h264'], timeout: 15)
endif
if conf.get('HAVE_V4L2_CTRL_HEVC') == 1
  fuzz_hevc_replay = executable('fuzz-hevc-replay', 'fuzz-hevc.c', 'fuzz-replay.c', 'fuzz-oracle.c', driver_sources,
    include_directories: fuzz_inc,
    c_args: fuzz_replay_c,
    dependencies: [libva_dep, libdrm_dep, threads_dep])
  test('fuzz-hevc', fuzz_hevc_replay, args: [fuzz_seed_root / 'hevc'], timeout: 15)
endif
if conf.get('HAVE_V4L2_CTRL_VP9') == 1
  fuzz_vp9_replay = executable('fuzz-vp9-replay', 'fuzz-vp9.c', 'fuzz-replay.c', 'fuzz-oracle.c', driver_sources,
    include_directories: fuzz_inc,
    c_args: fuzz_replay_c,
    dependencies: [libva_dep, libdrm_dep, threads_dep])
  test('fuzz-vp9', fuzz_vp9_replay, args: [fuzz_seed_root / 'vp9'], timeout: 15)
endif
fuzz_va_api_replay = executable('fuzz-va-api-replay', 'fuzz-va-api.c', 'fuzz-replay.c', 'fuzz-oracle.c', driver_sources,
  include_directories: fuzz_inc,
  c_args: fuzz_replay_c,
  dependencies: [libva_dep, libdrm_dep, threads_dep],
  link_args: fuzz_va_wrap)
test('fuzz-va-api', fuzz_va_api_replay, args: [fuzz_seed_root / 'va-api'], timeout: 15)
test('fuzz-budgets', fuzz_va_api_replay, args: ['--self-test'], timeout: 15)

fuzz_timeout_replay = executable('fuzz-timeout-replay', 'fuzz-timeout-mode.c',
  include_directories: fuzz_inc,
  c_args: fuzz_replay_c)
test('fuzz-timeout-replay', fuzz_timeout_replay, timeout: 5)
fuzz_timeout_campaign = executable('fuzz-timeout-campaign', 'fuzz-timeout-mode.c',
  include_directories: fuzz_inc,
  c_args: fuzz_campaign_def)
test('fuzz-timeout-campaign', fuzz_timeout_campaign, timeout: 5)
fuzz_replay_mismatch = executable('fuzz-replay-mismatch', 'fuzz-replay-mismatch.c', 'fuzz-oracle.c',
  include_directories: fuzz_inc,
  c_args: fuzz_replay_c)
test('fuzz-replay-mismatch', fuzz_replay_mismatch, timeout: 5)

if get_option('fuzzing').enabled()
  # libFuzzer is a C++ runtime. Combine it with the project's sanitizers so
  # clang sees one -fsanitize= list. Debian/Ubuntu needs compiler-rt
  # (libclang-rt-dev / libclang-rt-<ver>-dev) in addition to clang.
  fuzzer_sanitize = 'fuzzer'
  b_san = get_option('b_sanitize')
  if b_san != 'none'
    fuzzer_sanitize = 'fuzzer,' + b_san
  endif
  fuzzer_compile = ['-fsanitize=' + fuzzer_sanitize]
  fuzzer_lto = fuzzer_compile + ['-lstdc++']
  if not cc.links('''#include <stdint.h>
int LLVMFuzzerTestOneInput(const uint8_t *d, unsigned long n) { (void)d; (void)n; return 0; }
''', args: fuzzer_lto, name: 'libFuzzer')
    error('fuzzing was enabled with -Dfuzzing=enabled, but this compiler cannot link -fsanitize=fuzzer. Use clang, install compiler-rt (Debian/Ubuntu: libclang-rt-dev), and keep the replay binaries for gcc. See docs/FUZZING.md.')
  endif
  if conf.get('HAVE_V4L2_CTRL_H264') == 1
    executable('fuzz-h264', 'fuzz-h264.c', 'fuzz-oracle.c', driver_sources,
      include_directories: fuzz_inc,
      dependencies: [libva_dep, libdrm_dep, threads_dep],
      override_options: ['b_lundef=false'],
      c_args: fuzzer_compile + fuzz_campaign_def, link_args: fuzzer_lto)
  endif
  if conf.get('HAVE_V4L2_CTRL_HEVC') == 1
    executable('fuzz-hevc', 'fuzz-hevc.c', 'fuzz-oracle.c', driver_sources,
      include_directories: fuzz_inc,
      dependencies: [libva_dep, libdrm_dep, threads_dep],
      override_options: ['b_lundef=false'],
      c_args: fuzzer_compile + fuzz_campaign_def, link_args: fuzzer_lto)
  endif
  if conf.get('HAVE_V4L2_CTRL_VP9') == 1
    executable('fuzz-vp9', 'fuzz-vp9.c', 'fuzz-oracle.c', driver_sources,
      include_directories: fuzz_inc,
      dependencies: [libva_dep, libdrm_dep, threads_dep],
      override_options: ['b_lundef=false'],
      c_args: fuzzer_compile + fuzz_campaign_def, link_args: fuzzer_lto)
  endif
  executable('fuzz-va-api', 'fuzz-va-api.c', 'fuzz-oracle.c', driver_sources,
    include_directories: fuzz_inc,
    dependencies: [libva_dep, libdrm_dep, threads_dep],
    override_options: ['b_lundef=false'],
    c_args: fuzzer_compile + fuzz_campaign_def, link_args: fuzzer_lto + fuzz_va_wrap)
endif


if conf.get('HAVE_V4L2_PIX_FMT_P010') == 1 and (conf.get('HAVE_V4L2_CTRL_H264') == 1 or conf.get('HAVE_V4L2_CTRL_HEVC') == 1)
  surface_probe = executable('surface-probe', 'surface-probe.c', driver_sources,
    c_args: ['-UNDEBUG'],
    include_directories: [root_inc, include_directories('../src')],
    dependencies: [libva_dep, libdrm_dep, threads_dep],
    link_args: ['-Wl,--wrap=ioctl'])
  test('surface-probe', surface_probe, timeout: 10)
endif

early_export_backing = executable('early-export-backing', 'early-export-backing.c', driver_sources,
  c_args: ['-UNDEBUG'],
  include_directories: [root_inc, include_directories('../src')],
  dependencies: [libva_dep, libdrm_dep, threads_dep],
  link_args: ['-Wl,--wrap=ioctl'])
test('early-export-backing', early_export_backing, timeout: 10)

python3 = find_program('python3')
# Multi-process schedules: N worker processes of the stress harness
# behind a start barrier, aggregated and deadline-checked by the
# orchestrator. Its --self-test validates parsing, the independent
# digest derivation and (with the binary) real negative fixtures:
# stalled worker, inherited stdout holder and launch failure.
test('concurrent-processes', python3,
     args: [files('concurrent-process.py'), '--self-test',
            concurrent_stress], timeout: 120)
foreach n : ['1', '2', '4']
  test('concurrent-processes-' + n, python3,
       args: [files('concurrent-process.py'), concurrent_stress,
              '--processes', n, '--frames', '12', '--reps', '3',
              '--seed', '0xC0FFEE', '--deadline', '120'], timeout: 300)
endforeach
# ThreadSanitizer pass in a fresh sanitizer build; skips (77) with the
# reason when the toolchain or sandbox cannot run TSan.
test('concurrent-tsan', find_program('sh'),
     args: [files('concurrent-tsan.sh')], timeout: 600)
test('support-matrix', python3, args: files('support-matrix.py'), timeout: 30)
test('corpus-manifest', python3, args: [files('corpus.py'), 'self-test'], timeout: 60)
test('corpus-integrity', python3, args: files('corpus-integrity.py'), timeout: 30)
test('conformance-result', python3, args: [files('compare-results.py'), '--self-test'], timeout: 30)
test('conformance-runner', python3, args: [files('conformance-runner.py')], timeout: 30)
test('diag-schema', python3, args: [files('diag-schema.py'), diagnostics], timeout: 30)
# Offline differential checker for HEVC reference traces (issue #84); with
# the HEVC backend built it also validates the writer's real records.
if conf.get('HAVE_V4L2_CTRL_HEVC') == 1
  test('hevc-reftrace-check', python3,
       args: [files('hevc-reftrace-check.py'), '--self-test', hevc_reftrace], timeout: 30)
else
  test('hevc-reftrace-check', python3,
       args: [files('hevc-reftrace-check.py'), '--self-test'], timeout: 30)
endif
test('hwguard', python3, args: [files('hwguard.py'), '--self-test'], timeout: 60)
test('resource-churn', python3, args: [files('resource-churn.py'), 'self-test'], timeout: 30)
# The real FFmpeg workload is exercised only where its development libraries
# and encoders are installed; the core driver build has no FFmpeg dependency.
if find_program('ffmpeg', required: false).found() and dependency('libavformat', required: false).found() and dependency('libavcodec', required: false).found() and dependency('libavutil', required: false).found() and dependency('libswscale', required: false).found()
  test('resource-campaign', python3,
    args: files('resource-campaign-check.py'), timeout: 120)
  libav_deps = [dependency('libavformat'), dependency('libavcodec'),
                dependency('libavutil'), dependency('libswscale'), threads_dep]
  concurrent_va_worker = executable('concurrent-va-worker', 'concurrent-va-worker.c',
    dependencies: libav_deps)
  test('concurrent-va-software', python3,
       args: [files('concurrent-va-run.py'), '--self-test', concurrent_va_worker],
       timeout: 120)
  test('concurrent-va-negative', python3,
       args: [files('concurrent-va-test.py'), concurrent_va_worker], timeout: 30)
endif
test('rps-e-research', python3, args: files('rps-e-research.py'), timeout: 15)
test('hevc-concurrent', python3, args: [files('hevc-concurrent.py'), '--self-test'], timeout: 15)
test('hevc-concurrency-research', python3, args: files('hevc-concurrency-research.py'), timeout: 15)

# Offline fixtures and wiring audit for the CI aggregate gate.  The audit
# checks the actual .github/workflows/checks.yml, so every environment this
# suite runs in revalidates the required-check wiring.
test('ci-aggregate-fixtures', python3, args: [files('ci_check.py'), 'fixtures'], timeout: 30)
test('ci-workflow-audit', python3, args: [files('ci_check.py'), 'audit', '--workflow', workflow_yml], timeout: 15)
test('fuzz-provenance', python3, args: files('fuzz-provenance.py'), timeout: 15)

test('concurrent-process-cleanup', python3, args: files('concurrent-process-test.py'), timeout: 30)
test('concurrent-tsan-probe', python3, args: [files('concurrent-tsan.py'), '--self-test'], timeout: 30)
test('fuzz-campaign-runner', python3, args: [files('fuzz-campaign.py'), '--self-test'], timeout: 30)
test('h264-syntax-probe', python3, args: files('h264-syntax-probe-test.py'), timeout: 30)

# Published M1 evidence is verified offline; this never opens a decoder.
test('concurrency-hardware-evidence', python3,
     args: [files('../docs/concurrency-2026-09-18/verify.py'), '--self-test'], timeout: 30)

if conf.get('HAVE_V4L2_CTRL_AV1') == 1
  av1_reftrace = executable('av1-reftrace', 'av1-reftrace.c', driver_sources,
    c_args: ['-UNDEBUG'],
    include_directories: [root_inc, include_directories('../src')],
    dependencies: [libva_dep, libdrm_dep, threads_dep],
    link_args: ['-Wl,--wrap=ioctl', '-Wl,--wrap=poll', '-Wl,--wrap=__poll_chk'])
  foreach gate : ['gate-set', 'gate-empty', 'gate-unset']
    test('av1-reftrace-' + gate, av1_reftrace,
         args: [gate, 'off', meson.current_build_dir() / ('av1-' + gate)], timeout: 15)
  endforeach
  foreach case : ['grain-avd-display', 'grain-avd-inplace', 'grain-other-display',
                  'grain-no-control', 'grain-sequence-only', 'grain-other-inplace', 'basic', 'default', 'default-missing', 'default-foreign', 'failure',
                  'required-missing', 'required-foreign', 'required-failed', 'required-detached',
                  'primary-bounds', 'slot-bounds', 'valid-primary', 'key-default',
                  'intra-default', 'resilient-default', 'none-default']
    foreach tracing : ['on', 'off']
      test('av1-reftrace-' + case + '-' + tracing, av1_reftrace,
           args: [case, tracing, meson.current_build_dir() / ('av1-' + case + '-' + tracing + '.controls')], timeout: 15)
    endforeach
  endforeach
endif
