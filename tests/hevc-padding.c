/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Verify submitted controls and bytes with the existing in-memory device. */
#define main reftrace_main
#include "hevc-reftrace.c"
#undef main

static void padding_case(bool avd, bool annexb, unsigned padding)
{
    unsigned original_size[2], output_size[2];
    uint8_t expected[256];
    unsigned expected_size = 0;
    VASliceParameterBufferHEVC original[2];
    log_reset();
    setup(avd, V4L2_STATELESS_HEVC_DECODE_MODE_FRAME_BASED, 32);
    codec.start_code = annexb ? V4L2_STATELESS_HEVC_START_CODE_ANNEX_B :
                               V4L2_STATELESS_HEVC_START_CODE_NONE;
    picture_init(0, 0, true, false);
    for (unsigned i = 0; i < 2; i++) {
        add_slice(V4L2_HEVC_SLICE_TYPE_I, NULL, 0, NULL, 0);
        /* An escaped zero word ends in 0x03: retain it, including its zeros. */
        memcpy(slice_data + slice_data_used - 3, "\0\0\3", 3);
        original_size[i] = slices[i].slice_data_size;
        if (annexb) {
            memcpy(expected + expected_size, "\0\0\1", 3);
            expected_size += 3;
        }
        memcpy(expected + expected_size, slice_data + slices[i].slice_data_offset,
               original_size[i]);
        expected_size += original_size[i];
        output_size[i] = original_size[i] + (annexb ? 3 : 0);
        memset(slice_data + slice_data_used, 0, padding);
        slice_data_used += padding;
        slices[i].slice_data_size += padding;
        original[i] = slices[i];
    }
    assert(decode_picture() == VA_STATUS_SUCCESS);
    const struct v4l2_ctrl_hevc_slice_params *sp = NULL;
    uint32_t size = 0;
    for (unsigned i = 0; i < log_count; i++) {
        sp = captured_control(&log_[i], V4L2_CID_STATELESS_HEVC_SLICE_PARAMS, &size);
        if (sp) break;
    }
    assert(sp && size == 2 * sizeof(*sp));
    assert(sp[0].bit_size == output_size[0] * 8);
    assert(sp[1].bit_size == output_size[1] * 8);
    assert(last_output_bytesused == expected_size);
    assert(!memcmp(output_mem[0], expected, expected_size));
    assert(!memcmp(slices, original, sizeof(original)));
    teardown();
    log_reset();
}

static void rejection_order_case(bool avd, bool annexb, bool invalid_offset)
{
    VASliceParameterBufferHEVC original[2];
    const unsigned padding = 35;

    log_reset();
    setup(avd, V4L2_STATELESS_HEVC_DECODE_MODE_SLICE_BASED, 1);
    codec.start_code = annexb ? V4L2_STATELESS_HEVC_START_CODE_ANNEX_B :
                               V4L2_STATELESS_HEVC_START_CODE_NONE;
    picture_init(0, 0, true, false);
    add_slice(V4L2_HEVC_SLICE_TYPE_I, NULL, 0, NULL, 0);
    add_slice(V4L2_HEVC_SLICE_TYPE_I, NULL, 0, NULL, 0);
    unsigned trimmed_size = slices[1].slice_data_size;
    memset(slice_data + slice_data_used, 0, padding);
    slice_data_used += padding;
    slices[1].slice_data_size += padding;
    if (invalid_offset) {
        /* Pass the original bounds check, but point into trimmed padding. */
        slices[1].slice_data_byte_offset = trimmed_size + 1;
        assert(slices[1].slice_data_byte_offset <= slices[1].slice_data_size);
        assert(slices[1].slice_data_byte_offset > trimmed_size);
    }
    memcpy(original, slices, sizeof(original));
    unsigned queued_before = requests_queued;
    unsigned controls_before = log_count;
    VAStatus status = decode_picture();

    if (invalid_offset) {
        assert(status == VA_STATUS_ERROR_INVALID_BUFFER);
        /* The first slice filled the batch; rejecting the next slice must
         * not submit it, even though the next slice's header is valid. */
        assert(requests_queued == queued_before);
        assert(log_count == controls_before);
        assert(codec.num_slice_params == codec.max_slice_params);
        assert(codec.num_slices == 1);
    } else {
        /* Same padded input with a valid offset exercises the flush path. */
        assert(status == VA_STATUS_SUCCESS);
        assert(requests_queued == queued_before + 2);
        assert(log_count == controls_before + 2);
    }
    assert(!memcmp(slices, original, sizeof(original)));
    assert(!memcmp(codec.va_slices, original, sizeof(original)));
    teardown();
    log_reset();
}

int main(void)
{
    struct rlimit core = {0, 0};
    assert(!setrlimit(RLIMIT_CORE, &core));
    for (unsigned avd = 0; avd < 2; avd++)
        for (unsigned annexb = 0; annexb < 2; annexb++) {
            padding_case(avd, annexb, 0);
            padding_case(avd, annexb, 1);
            padding_case(avd, annexb, 35);
            rejection_order_case(avd, annexb, true);
            rejection_order_case(avd, annexb, false);
        }
    puts("HEVC padding: controls, multi-slice payload and client input preservation PASS");
    return 0;
}
