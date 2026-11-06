project('libva-v4l2_request', 'c',
        version: '1.3.r11',
        license: 'GPL-3.0',
        default_options: [
          'c_std=gnu11',
          'buildtype=debugoptimized',
          'warning_level=2',
          # debugoptimized keeps DWARF debug info, which embeds the absolute
          # source path of every compiled file (DW_AT_comp_dir/DW_AT_name).
          # Strip installed binaries by default to omit DWARF build paths;
          # the install smoke separately checks for embedded path strings.
          # The build-tree copy under
          # build/src/ stays unstripped, so LIBVA_DRIVERS_PATH=build/src
          # development keeps full debug symbols. Override with
          # `-Dstrip=false` (or `meson configure -Dstrip=false`) to install
          # an unstripped copy, e.g. for a distro -dbg package.
          'strip=true',
        ])

cc = meson.get_compiler('c')

# libva's pkg-config Version is the VA API version (libva 2.20 reports
# 1.20.0), so this floor means the library release libva >= 2.7.0: the oldest
# combination verified to configure, build and pass the offline suite
# (Ubuntu 20.04: libva 2.7, gcc 9, linux-libc-dev 5.4).  Code without its
# guards needs the DRM_PRIME_2 surface-export API set from libva 2.4
# (VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, vaExportSurfaceHandle,
# VA_EXPORT_SURFACE_SEPARATE_LAYERS); opt-in H.264 High 10 additionally
# needs libva 2.18 and compiles out below it.  Anything older fails right
# here with the version it found versus the floor.
libva_dep = dependency('libva', version: '>= 1.7.0')
libdrm_dep = dependency('libdrm')
threads_dep = dependency('threads')

# The driver talks straight to the kernel, make sure the uapi headers are
# recent enough for the stateless codec interfaces we support.  Each probe
# lists the first mainline kernel providing it (verified against the
# torvalds/linux tags): h264 5.11, vp8 5.13, mpeg2 5.14, vp9 5.17,
# hevc 6.0, av1 6.5; the P010 capture format and the HEVC decode-params
# member num_delta_pocs_of_ref_rps_idx arrived in 6.0 and 6.5.
# Codecs whose UAPI is absent are compiled out automatically; a codec enabled
# explicitly without its UAPI fails configuration naming the control.
codec_checks = {
  'h264':  ['V4L2_CID_STATELESS_H264_DECODE_MODE', '5.11'],
  'hevc':  ['V4L2_CID_STATELESS_HEVC_SPS',         '6.0'],
  'mpeg2': ['V4L2_CID_STATELESS_MPEG2_SEQUENCE',   '5.14'],
  'vp8':   ['V4L2_CID_STATELESS_VP8_FRAME',        '5.13'],
  'vp9':   ['V4L2_CID_STATELESS_VP9_FRAME',        '5.17'],
  'av1':   ['V4L2_CID_STATELESS_AV1_SEQUENCE',    '6.5'],
}

conf = configuration_data()
conf.set_quoted('V4L2R_VERSION', meson.project_version())
conf.set_quoted('V4L2R_VENDOR', get_option('vendor-string'))
codec_report = []
have_any_codec = false
foreach codec, check : codec_checks
  ctrl = check[0]
  first_kernel = check[1]
  want = get_option('codec_' + codec)
  have = cc.compiles('#include <linux/videodev2.h>\nint i = @0@;'.format(ctrl),
                     name: 'kernel supports stateless @0@'.format(codec))
  if want.enabled() and not have
    error('Codec "@0@" was enabled with -Dcodec_@0@=enabled, but the installed kernel UAPI headers do not provide @1@ (first available in Linux @2@). Install newer linux-libc-dev headers, or configure the codec as auto or disabled.'.format(codec, ctrl, first_kernel))
  endif
  codec_available = want.disabled() ? false : have
  conf.set10('HAVE_V4L2_CTRL_' + codec.to_upper(), codec_available)
  have_any_codec = have_any_codec or codec_available
  if want.disabled()
    codec_report += '@0@: disabled (-Dcodec_@0@=disabled)'.format(codec)
  elif not have
    codec_report += '@0@: not available (UAPI @1@ missing, first in Linux @2@)'.format(codec, ctrl, first_kernel)
  else
    codec_report += '@0@: @1@'.format(codec, want.enabled() ? 'enabled (forced)' : 'auto-detected')
  endif
endforeach
conf.set10('HAVE_V4L2R_CODECS', have_any_codec)

# The multi-slice hold-capture API pair (buffer flag and queue capability)
# arrived in Linux 5.9, before the first stateless codec pixelformat (H.264,
# 5.11): when it is missing no codec is compiled in, so the guarded uses in
# decode.c are unreachable rather than degraded.
have_m2m_hold = cc.compiles('#include <linux/videodev2.h>\nint hold = V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF;\nint cap = V4L2_BUF_CAP_SUPPORTS_M2M_HOLD_CAPTURE_BUF;',
                            name: 'kernel provides the M2M hold capture buffer API')
conf.set10('HAVE_V4L2_M2M_HOLD_CAPTURE_BUF', have_m2m_hold)
if not have_m2m_hold
  codec_report += 'm2m hold capture buffer: not available (V4L2_BUF_FLAG_M2M_HOLD_CAPTURE_BUF missing, first in Linux 5.9)'
endif

have_p010 = cc.compiles('#include <linux/videodev2.h>\nint i = V4L2_PIX_FMT_P010;',
                        name: 'kernel provides the P010 capture format')
conf.set10('HAVE_V4L2_PIX_FMT_P010', have_p010)
if not have_p010
  codec_report += 'p010 capture format: not available (V4L2_PIX_FMT_P010 missing, first in Linux 6.0)'
endif

conf.set10('HAVE_HEVC_DECODE_PARAMS_NUM_DELTA_POCS',
           cc.has_member('struct v4l2_ctrl_hevc_decode_params',
                         'num_delta_pocs_of_ref_rps_idx',
                         prefix: '#include <linux/videodev2.h>'))

summary({'codecs': '\n  '.join([''] + codec_report)})

configure_file(output: 'config.h', configuration: conf)
root_inc = include_directories('.')
workflow_yml = meson.current_source_dir() / '.github/workflows/checks.yml'

if get_option('driverdir') != ''
  driverdir = get_option('driverdir')
else
  driverdir = libva_dep.get_variable(pkgconfig: 'driverdir',
                                     default_value: get_option('prefix') / get_option('libdir') / 'dri')
endif

subdir('src')
subdir('tests')
