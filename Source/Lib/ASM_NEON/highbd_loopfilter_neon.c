/*
 * Copyright (c) 2022, Alliance for Open Media. All rights reserved.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#include <arm_neon.h>

#include "common_dsp_rtcd.h"
#include "definitions.h"
#include "mem_neon.h"
#include "transpose_neon.h"

static inline int16x4_t clip3_s16(const int16x4_t val, const int16x4_t low, const int16x4_t high) {
    return vmin_s16(vmax_s16(val, low), high);
}

static inline uint16x8_t convert_to_unsigned_pixel_u16(int16x8_t val, int bitdepth) {
    const int16x8_t  low  = vdupq_n_s16(0);
    const uint16x8_t high = vdupq_n_u16((1 << bitdepth) - 1);

    return vminq_u16(vreinterpretq_u16_s16(vmaxq_s16(val, low)), high);
}

// (abs(p1 - p0) > thresh) || (abs(q1 - q0) > thresh)
static inline uint16x4_t hev(const uint16x8_t abd_p0p1_q0q1, const uint16_t thresh) {
    const uint16x8_t a = vcgtq_u16(abd_p0p1_q0q1, vdupq_n_u16(thresh));
    return vorr_u16(vget_low_u16(a), vget_high_u16(a));
}

// abs(p0 - q0) * 2 + abs(p1 - q1) / 2 <= outer_thresh
static inline uint16x4_t outer_threshold(const uint16x4_t p1, const uint16x4_t p0, const uint16x4_t q0,
                                         const uint16x4_t q1, const uint16_t outer_thresh) {
    const uint16x4_t abd_p0q0    = vabd_u16(p0, q0);
    const uint16x4_t abd_p1q1    = vabd_u16(p1, q1);
    const uint16x4_t p0q0_double = vshl_n_u16(abd_p0q0, 1);
    const uint16x4_t p1q1_half   = vshr_n_u16(abd_p1q1, 1);
    const uint16x4_t sum         = vadd_u16(p0q0_double, p1q1_half);
    return vcle_u16(sum, vdup_n_u16(outer_thresh));
}

// abs(p1 - p0) <= inner_thresh && abs(q1 - q0) <= inner_thresh &&
//   outer_threshold()
static inline uint16x4_t needs_filter4(const uint16x8_t abd_p0p1_q0q1, const uint16_t inner_thresh,
                                       const uint16x4_t outer_mask) {
    const uint16x8_t a          = vcleq_u16(abd_p0p1_q0q1, vdupq_n_u16(inner_thresh));
    const uint16x4_t inner_mask = vand_u16(vget_low_u16(a), vget_high_u16(a));
    return vand_u16(inner_mask, outer_mask);
}

static inline void filter4_masks(const uint16x8_t p0q0, const uint16x8_t p1q1, const uint16_t hev_thresh,
                                 const uint16x4_t outer_mask, const uint16_t inner_thresh, uint16x4_t *const hev_mask,
                                 uint16x4_t *const needs_filter4_mask) {
    const uint16x8_t p0p1_q0q1 = vabdq_u16(p0q0, p1q1);
    // This includes cases where needs_filter4() is not true and so filter2
    // will not be applied.
    const uint16x4_t hev_tmp_mask = hev(p0p1_q0q1, hev_thresh);

    *needs_filter4_mask = needs_filter4(p0p1_q0q1, inner_thresh, outer_mask);

    // filter2 will only be applied if both needs_filter4() and hev() are true.
    *hev_mask = vand_u16(hev_tmp_mask, *needs_filter4_mask);
}

static inline void filter4(const uint16x8_t p0q0, const uint16x8_t p0q1, const uint16x8_t p1q1,
                           const uint16x4_t hev_mask, int bitdepth, uint16x8_t *const p1q1_result,
                           uint16x8_t *const p0q0_result) {
    const uint16x8_t q0p1 = vextq_u16(p0q0, p1q1, 4);
    // a = 3 * (q0 - p0) + Clip3(p1 - q1, min_signed_val, max_signed_val);
    // q0mp0 means "q0 minus p0".
    const int16x8_t q0mp0_p1mq1 = vreinterpretq_s16_u16(vsubq_u16(q0p1, p0q1));
    const int16x4_t q0mp0_3     = vmul_n_s16(vget_low_s16(q0mp0_p1mq1), 3);

    // If this is for filter2 then include |p1mq1|. Otherwise zero it.
    const int16x4_t min_signed_pixel = vdup_n_s16(-(1 << (bitdepth - 1)));
    const int16x4_t max_signed_pixel = vdup_n_s16((1 << (bitdepth - 1)) - 1);
    const int16x4_t p1mq1            = vget_high_s16(q0mp0_p1mq1);
    const int16x4_t p1mq1_saturated  = clip3_s16(p1mq1, min_signed_pixel, max_signed_pixel);
    const int16x4_t hev_option       = vand_s16(vreinterpret_s16_u16(hev_mask), p1mq1_saturated);

    const int16x4_t a = vadd_s16(q0mp0_3, hev_option);

    // We can not shift with rounding because the clamp comes *before* the
    // shifting.
    // a1 = Clip3(a + 4, min_signed_val, max_signed_val) >> 3;
    // a2 = Clip3(a + 3, min_signed_val, max_signed_val) >> 3;
    const int16x4_t plus_four  = clip3_s16(vadd_s16(a, vdup_n_s16(4)), min_signed_pixel, max_signed_pixel);
    const int16x4_t plus_three = clip3_s16(vadd_s16(a, vdup_n_s16(3)), min_signed_pixel, max_signed_pixel);
    const int16x4_t a1         = vshr_n_s16(plus_four, 3);
    const int16x4_t a2         = vshr_n_s16(plus_three, 3);

    // a3 = (a1 + 1) >> 1;
    const int16x4_t a3 = vrshr_n_s16(a1, 1);

    const int16x8_t a3_ma3  = vcombine_s16(a3, vneg_s16(a3));
    const int16x8_t p1q1_a3 = vaddq_s16(vreinterpretq_s16_u16(p1q1), a3_ma3);

    // Need to shift the second term or we end up with a2_ma2.
    const int16x8_t a2_ma1 = vcombine_s16(a2, vneg_s16(a1));
    const int16x8_t p0q0_a = vaddq_s16(vreinterpretq_s16_u16(p0q0), a2_ma1);
    *p1q1_result           = convert_to_unsigned_pixel_u16(p1q1_a3, bitdepth);
    *p0q0_result           = convert_to_unsigned_pixel_u16(p0q0_a, bitdepth);
}

void svt_aom_highbd_lpf_horizontal_4_neon(uint16_t *s, int pitch, const uint8_t *blimit, const uint8_t *limit,
                                          const uint8_t *thresh, int bd) {
    uint16x4_t src[4];
    load_u16_4x4(s - 2 * pitch, pitch, &src[0], &src[1], &src[2], &src[3]);

    // Adjust thresholds to bitdepth.
    const int        outer_thresh = *blimit << (bd - 8);
    const int        inner_thresh = *limit << (bd - 8);
    const int        hev_thresh   = *thresh << (bd - 8);
    const uint16x4_t outer_mask   = outer_threshold(src[0], src[1], src[2], src[3], outer_thresh);
    uint16x4_t       hev_mask;
    uint16x4_t       needs_filter4_mask;
    const uint16x8_t p0q0 = vcombine_u16(src[1], src[2]);
    const uint16x8_t p1q1 = vcombine_u16(src[0], src[3]);
    filter4_masks(p0q0, p1q1, hev_thresh, outer_mask, inner_thresh, &hev_mask, &needs_filter4_mask);

    if (vget_lane_u64(vreinterpret_u64_u16(needs_filter4_mask), 0) == 0) {
        // None of the values will be filtered.
        return;
    }

    // Copy the masks to the high bits for packed comparisons later.
    const uint16x8_t hev_mask_8           = vcombine_u16(hev_mask, hev_mask);
    const uint16x8_t needs_filter4_mask_8 = vcombine_u16(needs_filter4_mask, needs_filter4_mask);

    uint16x8_t       f_p1q1;
    uint16x8_t       f_p0q0;
    const uint16x8_t p0q1 = vcombine_u16(src[1], src[3]);
    filter4(p0q0, p0q1, p1q1, hev_mask, bd, &f_p1q1, &f_p0q0);

    // Already integrated the hev mask when calculating the filtered values.
    const uint16x8_t p0q0_output = vbslq_u16(needs_filter4_mask_8, f_p0q0, p0q0);

    // p1/q1 are unmodified if only hev() is true. This works because it was and'd
    // with |needs_filter4_mask| previously.
    const uint16x8_t p1q1_mask   = veorq_u16(hev_mask_8, needs_filter4_mask_8);
    const uint16x8_t p1q1_output = vbslq_u16(p1q1_mask, f_p1q1, p1q1);

    store_u16_4x4(s - 2 * pitch,
                  pitch,
                  vget_low_u16(p1q1_output),
                  vget_low_u16(p0q0_output),
                  vget_high_u16(p0q0_output),
                  vget_high_u16(p1q1_output));
}
void svt_aom_highbd_lpf_vertical_4_neon(uint16_t *s, int pitch, const uint8_t *blimit, const uint8_t *limit,
                                        const uint8_t *thresh, int bd) {
    // Offset by 2 uint16_t values to load from first p1 position.
    uint16x4_t src[4];
    load_u16_4x4(s - 2, pitch, &src[0], &src[1], &src[2], &src[3]);
    transpose_array_inplace_u16_4x4(src);

    // Adjust thresholds to bitdepth.
    const int        outer_thresh = *blimit << (bd - 8);
    const int        inner_thresh = *limit << (bd - 8);
    const int        hev_thresh   = *thresh << (bd - 8);
    const uint16x4_t outer_mask   = outer_threshold(src[0], src[1], src[2], src[3], outer_thresh);
    uint16x4_t       hev_mask;
    uint16x4_t       needs_filter4_mask;
    const uint16x8_t p0q0 = vcombine_u16(src[1], src[2]);
    const uint16x8_t p1q1 = vcombine_u16(src[0], src[3]);
    filter4_masks(p0q0, p1q1, hev_thresh, outer_mask, inner_thresh, &hev_mask, &needs_filter4_mask);

    if (vget_lane_u64(vreinterpret_u64_u16(needs_filter4_mask), 0) == 0) {
        // None of the values will be filtered.
        return;
    }

    // Copy the masks to the high bits for packed comparisons later.
    const uint16x8_t hev_mask_8           = vcombine_u16(hev_mask, hev_mask);
    const uint16x8_t needs_filter4_mask_8 = vcombine_u16(needs_filter4_mask, needs_filter4_mask);

    uint16x8_t       f_p1q1;
    uint16x8_t       f_p0q0;
    const uint16x8_t p0q1 = vcombine_u16(src[1], src[3]);
    filter4(p0q0, p0q1, p1q1, hev_mask, bd, &f_p1q1, &f_p0q0);

    // Already integrated the hev mask when calculating the filtered values.
    const uint16x8_t p0q0_output = vbslq_u16(needs_filter4_mask_8, f_p0q0, p0q0);

    // p1/q1 are unmodified if only hev() is true. This works because it was and'd
    // with |needs_filter4_mask| previously.
    const uint16x8_t p1q1_mask   = veorq_u16(hev_mask_8, needs_filter4_mask_8);
    const uint16x8_t p1q1_output = vbslq_u16(p1q1_mask, f_p1q1, p1q1);

    uint16x4_t output[4] = {
        vget_low_u16(p1q1_output),
        vget_low_u16(p0q0_output),
        vget_high_u16(p0q0_output),
        vget_high_u16(p1q1_output),
    };
    transpose_array_inplace_u16_4x4(output);

    store_u16_4x4(s - 2, pitch, output[0], output[1], output[2], output[3]);
}

// abs(p1 - p0) <= flat_thresh && abs(q1 - q0) <= flat_thresh &&
//   abs(p2 - p0) <= flat_thresh && abs(q2 - q0) <= flat_thresh
// |flat_thresh| == 4 for 10 bit decode.
static inline uint16x4_t is_flat3(const uint16x8_t abd_p0p1_q0q1, const uint16x8_t abd_p0p2_q0q2, const int bitdepth) {
    const int        flat_thresh = 1 << (bitdepth - 8);
    const uint16x8_t a           = vmaxq_u16(abd_p0p1_q0q1, abd_p0p2_q0q2);
    const uint16x8_t b           = vcleq_u16(a, vdupq_n_u16(flat_thresh));
    return vand_u16(vget_low_u16(b), vget_high_u16(b));
}

// abs(p2 - p1) <= inner_thresh && abs(p1 - p0) <= inner_thresh &&
//   abs(q1 - q0) <= inner_thresh && abs(q2 - q1) <= inner_thresh &&
//   outer_threshold()
static inline uint16x4_t needs_filter6(const uint16x8_t abd_p0p1_q0q1, const uint16x8_t abd_p1p2_q1q2,
                                       const uint16_t inner_thresh, const uint16x4_t outer_mask) {
    const uint16x8_t a          = vmaxq_u16(abd_p0p1_q0q1, abd_p1p2_q1q2);
    const uint16x8_t b          = vcleq_u16(a, vdupq_n_u16(inner_thresh));
    const uint16x4_t inner_mask = vand_u16(vget_low_u16(b), vget_high_u16(b));
    return vand_u16(inner_mask, outer_mask);
}

static inline void filter6_masks(const uint16x8_t p2q2, const uint16x8_t p1q1, const uint16x8_t p0q0,
                                 const uint16_t hev_thresh, const uint16x4_t outer_mask, const uint16_t inner_thresh,
                                 const int bitdepth, uint16x4_t *const needs_filter6_mask,
                                 uint16x4_t *const is_flat3_mask, uint16x4_t *const hev_mask) {
    const uint16x8_t abd_p0p1_q0q1 = vabdq_u16(p0q0, p1q1);
    *hev_mask                      = hev(abd_p0p1_q0q1, hev_thresh);
    *is_flat3_mask                 = is_flat3(abd_p0p1_q0q1, vabdq_u16(p0q0, p2q2), bitdepth);
    *needs_filter6_mask            = needs_filter6(abd_p0p1_q0q1, vabdq_u16(p1q1, p2q2), inner_thresh, outer_mask);
}

static inline void filter6(const uint16x8_t p2q2, const uint16x8_t p1q1, const uint16x8_t p0q0,
                           uint16x8_t *const p1q1_output, uint16x8_t *const p0q0_output) {
    // Sum p1 and q1 output from opposite directions.
    // The formula is regrouped to allow 3 doubling operations to be combined.
    //
    // p1 = (3 * p2) + (2 * p1) + (2 * p0) + q0
    //      ^^^^^^^^
    // q1 = p0 + (2 * q0) + (2 * q1) + (3 * q2)
    //                                 ^^^^^^^^
    // p1q1 = p2q2 + 2 * (p2q2 + p1q1 + p0q0) + q0p0
    //                    ^^^^^^^^^^^
    uint16x8_t sum = vaddq_u16(p2q2, p1q1);

    // p1q1 = p2q2 + 2 * (p2q2 + p1q1 + p0q0) + q0p0
    //                                ^^^^^^
    sum = vaddq_u16(sum, p0q0);

    // p1q1 = p2q2 + 2 * (p2q2 + p1q1 + p0q0) + q0p0
    //        ^^^^^^                          ^^^^^^
    // Should dual issue with the left shift.
    const uint16x8_t q0p0      = vextq_u16(p0q0, p0q0, 4);
    const uint16x8_t outer_sum = vaddq_u16(p2q2, q0p0);
    // p1q1 = p2q2 + 2 * (p2q2 + p1q1 + p0q0) + q0p0
    //        ^^^^^^^^^^^                       ^^^^
    sum = vmlaq_n_u16(outer_sum, sum, 2);

    *p1q1_output = vrshrq_n_u16(sum, 3);

    // Convert to p0 and q0 output:
    // p0 = p1 - (2 * p2) + q0 + q1
    // q0 = q1 - (2 * q2) + p0 + p1
    // p0q0 = p1q1 - (2 * p2q2) + q0p0 + q1p1
    //        ^^^^^^^^^^^^^^^^^
    sum                   = vmlsq_n_u16(sum, p2q2, 2);
    const uint16x8_t q1p1 = vextq_u16(p1q1, p1q1, 4);
    sum                   = vaddq_u16(sum, vaddq_u16(q0p0, q1p1));

    *p0q0_output = vrshrq_n_u16(sum, 3);
}

void svt_aom_highbd_lpf_horizontal_6_neon(uint16_t *s, int pitch, const uint8_t *blimit, const uint8_t *limit,
                                          const uint8_t *thresh, int bd) {
    uint16x4_t src[6];
    load_u16_4x6(s - 3 * pitch, pitch, &src[0], &src[1], &src[2], &src[3], &src[4], &src[5]);

    // Adjust thresholds to bitdepth.
    const int        outer_thresh = *blimit << (bd - 8);
    const int        inner_thresh = *limit << (bd - 8);
    const int        hev_thresh   = *thresh << (bd - 8);
    const uint16x4_t outer_mask   = outer_threshold(src[1], src[2], src[3], src[4], outer_thresh);
    uint16x4_t       hev_mask;
    uint16x4_t       needs_filter_mask;
    uint16x4_t       is_flat3_mask;
    const uint16x8_t p0q0 = vcombine_u16(src[2], src[3]);
    const uint16x8_t p1q1 = vcombine_u16(src[1], src[4]);
    const uint16x8_t p2q2 = vcombine_u16(src[0], src[5]);
    filter6_masks(
        p2q2, p1q1, p0q0, hev_thresh, outer_mask, inner_thresh, bd, &needs_filter_mask, &is_flat3_mask, &hev_mask);

    if (vget_lane_u64(vreinterpret_u64_u16(needs_filter_mask), 0) == 0) {
        // None of the values will be filtered.
        return;
    }

    uint16x8_t p0q0_output, p1q1_output;
    uint16x8_t f6_p1q1, f6_p0q0;
    // Not needing filter4() at all is a very common case, so isolate it to avoid needlessly computing filter4().
    if (vget_lane_s64(vreinterpret_s64_u16(is_flat3_mask), 0) == -1 &&
        vget_lane_s64(vreinterpret_s64_u16(needs_filter_mask), 0) == -1) {
        filter6(p2q2, p1q1, p0q0, &f6_p1q1, &f6_p0q0);
        p1q1_output = f6_p1q1;
        p0q0_output = f6_p0q0;
    } else {
        // Copy the masks to the high bits for packed comparisons later.
        const uint16x8_t hev_mask_8          = vcombine_u16(hev_mask, hev_mask);
        const uint16x8_t is_flat3_mask_8     = vcombine_u16(is_flat3_mask, is_flat3_mask);
        const uint16x8_t needs_filter_mask_8 = vcombine_u16(needs_filter_mask, needs_filter_mask);

        uint16x8_t       f4_p1q1;
        uint16x8_t       f4_p0q0;
        const uint16x8_t p0q1 = vcombine_u16(src[2], src[4]);
        filter4(p0q0, p0q1, p1q1, hev_mask, bd, &f4_p1q1, &f4_p0q0);
        f4_p1q1 = vbslq_u16(hev_mask_8, p1q1, f4_p1q1);

        // Because we did not return after testing |needs_filter_mask| we know it is
        // nonzero. |is_flat3_mask| controls whether the needed filter is filter4 or
        // filter6. Therefore if it is false when |needs_filter_mask| is true, filter6
        // output is not used.
        const uint64x1_t need_filter6 = vreinterpret_u64_u16(is_flat3_mask);
        if (vget_lane_u64(need_filter6, 0) == 0) {
            // filter6() does not apply, but filter4() applies to one or more values.
            p0q0_output = p0q0;
            p1q1_output = vbslq_u16(needs_filter_mask_8, f4_p1q1, p1q1);
            p0q0_output = vbslq_u16(needs_filter_mask_8, f4_p0q0, p0q0);
        } else {
            filter6(p2q2, p1q1, p0q0, &f6_p1q1, &f6_p0q0);
            p1q1_output = vbslq_u16(is_flat3_mask_8, f6_p1q1, f4_p1q1);
            p1q1_output = vbslq_u16(needs_filter_mask_8, p1q1_output, p1q1);
            p0q0_output = vbslq_u16(is_flat3_mask_8, f6_p0q0, f4_p0q0);
            p0q0_output = vbslq_u16(needs_filter_mask_8, p0q0_output, p0q0);
        }
    }

    store_u16_4x4(s - 2 * pitch,
                  pitch,
                  vget_low_u16(p1q1_output),
                  vget_low_u16(p0q0_output),
                  vget_high_u16(p0q0_output),
                  vget_high_u16(p1q1_output));
}

void svt_aom_highbd_lpf_vertical_6_neon(uint16_t *s, int pitch, const uint8_t *blimit, const uint8_t *limit,
                                        const uint8_t *thresh, int bd) {
    // Overread by 2 values. These overreads become the high halves of src_raw[2]
    // and src_raw[3] after transpose.
    uint16x8_t src_raw[4];
    load_u16_8x4(s - 3, pitch, &src_raw[0], &src_raw[1], &src_raw[2], &src_raw[3]);

    transpose_array_inplace_u16_4x8(src_raw);
    // p2, p1, p0, q0, q1, q2
    const uint16x4_t src[6] = {
        vget_low_u16(src_raw[0]),
        vget_low_u16(src_raw[1]),
        vget_low_u16(src_raw[2]),
        vget_low_u16(src_raw[3]),
        vget_high_u16(src_raw[0]),
        vget_high_u16(src_raw[1]),
    };

    // Adjust thresholds to bitdepth.
    const int        outer_thresh = *blimit << (bd - 8);
    const int        inner_thresh = *limit << (bd - 8);
    const int        hev_thresh   = *thresh << (bd - 8);
    const uint16x4_t outer_mask   = outer_threshold(src[1], src[2], src[3], src[4], outer_thresh);
    uint16x4_t       hev_mask;
    uint16x4_t       needs_filter_mask;
    uint16x4_t       is_flat3_mask;
    const uint16x8_t p0q0 = vcombine_u16(src[2], src[3]);
    const uint16x8_t p1q1 = vcombine_u16(src[1], src[4]);
    const uint16x8_t p2q2 = vcombine_u16(src[0], src[5]);
    filter6_masks(
        p2q2, p1q1, p0q0, hev_thresh, outer_mask, inner_thresh, bd, &needs_filter_mask, &is_flat3_mask, &hev_mask);

    if (vget_lane_u64(vreinterpret_u64_u16(needs_filter_mask), 0) == 0) {
        // None of the values will be filtered.
        return;
    }

    uint16x8_t p0q0_output, p1q1_output;
    // Because we did not return after testing |needs_filter_mask| we know it is
    // nonzero. |is_flat3_mask| controls whether the needed filter is filter4 or
    // filter6. Therefore if it is false when |needs_filter_mask| is true, filter6
    // output is not used.
    uint16x8_t       f6_p1q1, f6_p0q0;
    const uint64x1_t need_filter6 = vreinterpret_u64_u16(is_flat3_mask);
    // Not needing filter4() at all is a very common case, so isolate it to avoid needlessly computing filter4().
    if (vget_lane_s64(vreinterpret_s64_u16(is_flat3_mask), 0) == -1 &&
        vget_lane_s64(vreinterpret_s64_u16(needs_filter_mask), 0) == -1) {
        filter6(p2q2, p1q1, p0q0, &f6_p1q1, &f6_p0q0);
        p1q1_output = f6_p1q1;
        p0q0_output = f6_p0q0;
    } else {
        // Copy the masks to the high bits for packed comparisons later.
        const uint16x8_t hev_mask_8          = vcombine_u16(hev_mask, hev_mask);
        const uint16x8_t is_flat3_mask_8     = vcombine_u16(is_flat3_mask, is_flat3_mask);
        const uint16x8_t needs_filter_mask_8 = vcombine_u16(needs_filter_mask, needs_filter_mask);

        uint16x8_t f4_p1q1;
        uint16x8_t f4_p0q0;
        // ZIP1 p0q0, p1q1 may perform better here.
        const uint16x8_t p0q1 = vcombine_u16(src[2], src[4]);
        filter4(p0q0, p0q1, p1q1, hev_mask, bd, &f4_p1q1, &f4_p0q0);
        f4_p1q1 = vbslq_u16(hev_mask_8, p1q1, f4_p1q1);
        if (vget_lane_u64(need_filter6, 0) == 0) {
            // filter6() does not apply, but filter4() applies to one or more values.
            p0q0_output = p0q0;
            p1q1_output = vbslq_u16(needs_filter_mask_8, f4_p1q1, p1q1);
            p0q0_output = vbslq_u16(needs_filter_mask_8, f4_p0q0, p0q0);
        } else {
            filter6(p2q2, p1q1, p0q0, &f6_p1q1, &f6_p0q0);
            p1q1_output = vbslq_u16(is_flat3_mask_8, f6_p1q1, f4_p1q1);
            p1q1_output = vbslq_u16(needs_filter_mask_8, p1q1_output, p1q1);
            p0q0_output = vbslq_u16(is_flat3_mask_8, f6_p0q0, f4_p0q0);
            p0q0_output = vbslq_u16(needs_filter_mask_8, p0q0_output, p0q0);
        }
    }

    uint16x4_t output[4] = {
        vget_low_u16(p1q1_output),
        vget_low_u16(p0q0_output),
        vget_high_u16(p0q0_output),
        vget_high_u16(p1q1_output),
    };
    transpose_array_inplace_u16_4x4(output);

    store_u16_4x4(s - 2, pitch, output[0], output[1], output[2], output[3]);
}

// abs(p3 - p2) <= inner_thresh && abs(p2 - p1) <= inner_thresh &&
//   abs(p1 - p0) <= inner_thresh && abs(q1 - q0) <= inner_thresh &&
//   abs(q2 - q1) <= inner_thresh && abs(q3 - q2) <= inner_thresh
//   outer_threshold()
static inline uint16x4_t needs_filter8(const uint16x8_t abd_p0p1_q0q1, const uint16x8_t abd_p1p2_q1q2,
                                       const uint16x8_t abd_p2p3_q2q3, const uint16_t inner_thresh,
                                       const uint16x4_t outer_mask) {
    const uint16x8_t a          = vmaxq_u16(abd_p0p1_q0q1, abd_p1p2_q1q2);
    const uint16x8_t b          = vmaxq_u16(a, abd_p2p3_q2q3);
    const uint16x8_t c          = vcleq_u16(b, vdupq_n_u16(inner_thresh));
    const uint16x4_t inner_mask = vand_u16(vget_low_u16(c), vget_high_u16(c));
    return vand_u16(inner_mask, outer_mask);
}

// is_flat4 uses N=1, IsFlatOuter4 uses N=4.
// abs(p[N] - p0) <= flat_thresh && abs(q[N] - q0) <= flat_thresh &&
//   abs(p[N+1] - p0) <= flat_thresh && abs(q[N+1] - q0) <= flat_thresh &&
//   abs(p[N+2] - p0) <= flat_thresh && abs(q[N+1] - q0) <= flat_thresh
// |flat_thresh| == 4 for 10 bit decode.
static inline uint16x4_t is_flat4(const uint16x8_t abd_pnp0_qnq0, const uint16x8_t abd_pn1p0_qn1q0,
                                  const uint16x8_t abd_pn2p0_qn2q0, const int bitdepth) {
    const int        flat_thresh = 1 << (bitdepth - 8);
    const uint16x8_t a           = vmaxq_u16(abd_pnp0_qnq0, abd_pn1p0_qn1q0);
    const uint16x8_t b           = vmaxq_u16(a, abd_pn2p0_qn2q0);
    const uint16x8_t c           = vcleq_u16(b, vdupq_n_u16(flat_thresh));
    return vand_u16(vget_low_u16(c), vget_high_u16(c));
}

static inline void filter8_masks(const uint16x8_t p3q3, const uint16x8_t p2q2, const uint16x8_t p1q1,
                                 const uint16x8_t p0q0, const uint16_t hev_thresh, const uint16x4_t outer_mask,
                                 const uint16_t inner_thresh, const int bitdepth, uint16x4_t *const needs_filter8_mask,
                                 uint16x4_t *const is_flat4_mask, uint16x4_t *const hev_mask) {
    const uint16x8_t abd_p0p1_q0q1 = vabdq_u16(p0q0, p1q1);
    *hev_mask                      = hev(abd_p0p1_q0q1, hev_thresh);
    const uint16x4_t v_is_flat4    = is_flat4(abd_p0p1_q0q1, vabdq_u16(p0q0, p2q2), vabdq_u16(p0q0, p3q3), bitdepth);
    *needs_filter8_mask            = needs_filter8(
        abd_p0p1_q0q1, vabdq_u16(p1q1, p2q2), vabdq_u16(p2q2, p3q3), inner_thresh, outer_mask);
    // |is_flat4_mask| is used to decide where to use the result of filter8.
    // In rare cases, |is_flat4| can be true where |needs_filter8_mask| is false,
    // overriding the question of whether to use filter8. Because filter4 doesn't
    // apply to p2q2, |is_flat4_mask| chooses directly between filter8 and the
    // source value. To be correct, the mask must account for this override.
    *is_flat4_mask = vand_u16(v_is_flat4, *needs_filter8_mask);
}

static inline void filter8(const uint16x8_t p3q3, const uint16x8_t p2q2, const uint16x8_t p1q1, const uint16x8_t p0q0,
                           uint16x8_t *const p2q2_output, uint16x8_t *const p1q1_output,
                           uint16x8_t *const p0q0_output) {
    // Sum p2 and q2 output from opposite directions.
    // The formula is regrouped to allow 2 doubling operations to be combined.
    // p2 = (3 * p3) + (2 * p2) + p1 + p0 + q0
    //      ^^^^^^^^
    // q2 = p0 + q0 + q1 + (2 * q2) + (3 * q3)
    //                                ^^^^^^^^
    // p2q2 = p3q3 + 2 * (p3q3 + p2q2) + p1q1 + p0q0 + q0p0
    //                    ^^^^^^^^^^^
    const uint16x8_t p23q23 = vaddq_u16(p3q3, p2q2);

    // Add two other terms to make dual issue with shift more likely.
    // p2q2 = p3q3 + 2 * (p3q3 + p2q2) + p1q1 + p0q0 + q0p0
    //                                   ^^^^^^^^^^^
    const uint16x8_t p01q01 = vaddq_u16(p0q0, p1q1);

    // p2q2 = p3q3 + 2 * (p3q3 + p2q2) + p1q1 + p0q0 + q0p0
    //               ^^^^^             ^^^^^^^^^^^^^
    uint16x8_t sum = vmlaq_n_u16(p01q01, p23q23, 2);

    // p2q2 = p3q3 + 2 * (p3q3 + p2q2) + p1q1 + p0q0 + q0p0
    //        ^^^^^^
    sum = vaddq_u16(sum, p3q3);

    // p2q2 = p3q3 + 2 * (p3q3 + p2q2) + p1q1 + p0q0 + q0p0
    //                                               ^^^^^^
    const uint16x8_t q0p0 = vextq_u16(p0q0, p0q0, 4);
    sum                   = vaddq_u16(sum, q0p0);

    *p2q2_output = vrshrq_n_u16(sum, 3);

    // Convert to p1 and q1 output:
    // p1 = p2 - p3 - p2 + p1 + q1
    // q1 = q2 - q3 - q2 + q0 + p1
    sum                   = vsubq_u16(sum, p23q23);
    const uint16x8_t q1p1 = vextq_u16(p1q1, p1q1, 4);
    sum                   = vaddq_u16(sum, vaddq_u16(p1q1, q1p1));

    *p1q1_output = vrshrq_n_u16(sum, 3);

    // Convert to p0 and q0 output:
    // p0 = p1 - p3 - p1 + p0 + q2
    // q0 = q1 - q3 - q1 + q0 + p2
    sum                   = vsubq_u16(sum, vaddq_u16(p3q3, p1q1));
    const uint16x8_t q2p2 = vextq_u16(p2q2, p2q2, 4);
    sum                   = vaddq_u16(sum, vaddq_u16(p0q0, q2p2));

    *p0q0_output = vrshrq_n_u16(sum, 3);
}

void svt_aom_highbd_lpf_horizontal_8_neon(uint16_t *s, int pitch, const uint8_t *blimit, const uint8_t *limit,
                                          const uint8_t *thresh, int bd) {
    uint16x4_t src[8];
    load_u16_4x8(s - 4 * pitch, pitch, &src[0], &src[1], &src[2], &src[3], &src[4], &src[5], &src[6], &src[7]);

    // Adjust thresholds to bitdepth.
    const int        outer_thresh = *blimit << (bd - 8);
    const int        inner_thresh = *limit << (bd - 8);
    const int        hev_thresh   = *thresh << (bd - 8);
    const uint16x4_t outer_mask   = outer_threshold(src[2], src[3], src[4], src[5], outer_thresh);
    uint16x4_t       hev_mask;
    uint16x4_t       needs_filter_mask;
    uint16x4_t       is_flat4_mask;
    const uint16x8_t p0q0 = vcombine_u16(src[3], src[4]);
    const uint16x8_t p1q1 = vcombine_u16(src[2], src[5]);
    const uint16x8_t p2q2 = vcombine_u16(src[1], src[6]);
    const uint16x8_t p3q3 = vcombine_u16(src[0], src[7]);
    filter8_masks(p3q3,
                  p2q2,
                  p1q1,
                  p0q0,
                  hev_thresh,
                  outer_mask,
                  inner_thresh,
                  bd,
                  &needs_filter_mask,
                  &is_flat4_mask,
                  &hev_mask);

    if (vget_lane_u64(vreinterpret_u64_u16(needs_filter_mask), 0) == 0) {
        // None of the values will be filtered.
        return;
    }

    uint16x8_t p0q0_output, p1q1_output, p2q2_output;
    uint16x8_t f8_p2q2, f8_p1q1, f8_p0q0;
    // Not needing filter4() at all is a very common case, so isolate it to avoid needlessly computing filter4().
    if (vget_lane_s64(vreinterpret_s64_u16(is_flat4_mask), 0) == -1 &&
        vget_lane_s64(vreinterpret_s64_u16(needs_filter_mask), 0) == -1) {
        filter8(p3q3, p2q2, p1q1, p0q0, &f8_p2q2, &f8_p1q1, &f8_p0q0);
        p2q2_output = f8_p2q2;
        p1q1_output = f8_p1q1;
        p0q0_output = f8_p0q0;
    } else {
        // Copy the masks to the high bits for packed comparisons later.
        const uint16x8_t hev_mask_8          = vcombine_u16(hev_mask, hev_mask);
        const uint16x8_t needs_filter_mask_8 = vcombine_u16(needs_filter_mask, needs_filter_mask);

        uint16x8_t       f4_p1q1;
        uint16x8_t       f4_p0q0;
        const uint16x8_t p0q1 = vcombine_u16(src[3], src[5]);
        filter4(p0q0, p0q1, p1q1, hev_mask, bd, &f4_p1q1, &f4_p0q0);
        f4_p1q1 = vbslq_u16(hev_mask_8, p1q1, f4_p1q1);

        // Because we did not return after testing |needs_filter_mask| we know it is
        // nonzero. |is_flat4_mask| controls whether the needed filter is filter4 or
        // filter8. Therefore if it is false when |needs_filter_mask| is true, filter8
        // output is not used.
        const uint64x1_t need_filter8 = vreinterpret_u64_u16(is_flat4_mask);
        if (vget_lane_u64(need_filter8, 0) == 0) {
            // filter8() does not apply, but filter4() applies to one or more values.
            p2q2_output = p2q2;
            p1q1_output = vbslq_u16(needs_filter_mask_8, f4_p1q1, p1q1);
            p0q0_output = vbslq_u16(needs_filter_mask_8, f4_p0q0, p0q0);
        } else {
            const uint16x8_t is_flat4_mask_8 = vcombine_u16(is_flat4_mask, is_flat4_mask);
            filter8(p3q3, p2q2, p1q1, p0q0, &f8_p2q2, &f8_p1q1, &f8_p0q0);
            p2q2_output = vbslq_u16(is_flat4_mask_8, f8_p2q2, p2q2);
            p1q1_output = vbslq_u16(is_flat4_mask_8, f8_p1q1, f4_p1q1);
            p1q1_output = vbslq_u16(needs_filter_mask_8, p1q1_output, p1q1);
            p0q0_output = vbslq_u16(is_flat4_mask_8, f8_p0q0, f4_p0q0);
            p0q0_output = vbslq_u16(needs_filter_mask_8, p0q0_output, p0q0);
        }
    }

    store_u16_4x6(s - 3 * pitch,
                  pitch,
                  vget_low_u16(p2q2_output),
                  vget_low_u16(p1q1_output),
                  vget_low_u16(p0q0_output),
                  vget_high_u16(p0q0_output),
                  vget_high_u16(p1q1_output),
                  vget_high_u16(p2q2_output));
}

static const uint8_t rev_low_half_tbl[16] = {6, 7, 4, 5, 2, 3, 0, 1, 8, 9, 10, 11, 12, 13, 14, 15};

static inline uint16x8_t reverse_low_half(const uint16x8_t a) {
    uint8x16_t idx = vld1q_u8(rev_low_half_tbl);
    return vreinterpretq_u16_u8(vqtbl1q_u8(vreinterpretq_u8_u16(a), idx));
}

void svt_aom_highbd_lpf_vertical_8_neon(uint16_t *s, int pitch, const uint8_t *blimit, const uint8_t *limit,
                                        const uint8_t *thresh, int bd) {
    // src_raw[n] contains p3, p2, p1, p0, q0, q1, q2, q3 for row n.
    // To get desired pairs after transpose, one half should be reversed.
    uint16x8_t src[4];
    load_u16_8x4(s - 4, pitch, &src[0], &src[1], &src[2], &src[3]);

    // src[0] = p0q0
    // src[1] = p1q1
    // src[2] = p2q2
    // src[3] = p3q3
    loop_filter_transpose_u16_4x8q(src);

    // Adjust thresholds to bitdepth.
    const int        outer_thresh = *blimit << (bd - 8);
    const int        inner_thresh = *limit << (bd - 8);
    const int        hev_thresh   = *thresh << (bd - 8);
    const uint16x4_t outer_mask   = outer_threshold(
        vget_low_u16(src[1]), vget_low_u16(src[0]), vget_high_u16(src[0]), vget_high_u16(src[1]), outer_thresh);
    uint16x4_t       hev_mask;
    uint16x4_t       needs_filter_mask;
    uint16x4_t       is_flat4_mask;
    const uint16x8_t p0q0 = src[0];
    const uint16x8_t p1q1 = src[1];
    const uint16x8_t p2q2 = src[2];
    const uint16x8_t p3q3 = src[3];
    filter8_masks(p3q3,
                  p2q2,
                  p1q1,
                  p0q0,
                  hev_thresh,
                  outer_mask,
                  inner_thresh,
                  bd,
                  &needs_filter_mask,
                  &is_flat4_mask,
                  &hev_mask);

    if (vget_lane_u64(vreinterpret_u64_u16(needs_filter_mask), 0) == 0) {
        // None of the values will be filtered.
        return;
    }

    uint16x8_t p0q0_output, p1q1_output, p2q2_output;
    uint16x8_t f8_p2q2, f8_p1q1, f8_p0q0;
    // Not needing filter4() at all is a very common case, so isolate it to avoid needlessly computing filter4().
    if (vget_lane_s64(vreinterpret_s64_u16(is_flat4_mask), 0) == -1 &&
        vget_lane_s64(vreinterpret_s64_u16(needs_filter_mask), 0) == -1) {
        filter8(p3q3, p2q2, p1q1, p0q0, &f8_p2q2, &f8_p1q1, &f8_p0q0);
        p2q2_output = f8_p2q2;
        p1q1_output = f8_p1q1;
        p0q0_output = f8_p0q0;
    } else {
        // Copy the masks to the high bits for packed comparisons later.
        const uint16x8_t hev_mask_8          = vcombine_u16(hev_mask, hev_mask);
        const uint16x8_t needs_filter_mask_8 = vcombine_u16(needs_filter_mask, needs_filter_mask);

        uint16x8_t       f4_p1q1;
        uint16x8_t       f4_p0q0;
        const uint16x8_t p0q1 = vcombine_u16(vget_low_u16(p0q0), vget_high_u16(p1q1));
        filter4(p0q0, p0q1, p1q1, hev_mask, bd, &f4_p1q1, &f4_p0q0);
        f4_p1q1 = vbslq_u16(hev_mask_8, p1q1, f4_p1q1);

        // Because we did not return after testing |needs_filter_mask| we know it is
        // nonzero. |is_flat4_mask| controls whether the needed filter is filter4 or
        // filter8. Therefore if it is false when |needs_filter_mask| is true, filter8
        // output is not used.
        const uint64x1_t need_filter8 = vreinterpret_u64_u16(is_flat4_mask);
        if (vget_lane_u64(need_filter8, 0) == 0) {
            // filter8() does not apply, but filter4() applies to one or more values.
            p2q2_output = p2q2;
            p1q1_output = vbslq_u16(needs_filter_mask_8, f4_p1q1, p1q1);
            p0q0_output = vbslq_u16(needs_filter_mask_8, f4_p0q0, p0q0);
        } else {
            const uint16x8_t is_flat4_mask_8 = vcombine_u16(is_flat4_mask, is_flat4_mask);
            filter8(p3q3, p2q2, p1q1, p0q0, &f8_p2q2, &f8_p1q1, &f8_p0q0);
            p2q2_output = vbslq_u16(is_flat4_mask_8, f8_p2q2, p2q2);
            p1q1_output = vbslq_u16(is_flat4_mask_8, f8_p1q1, f4_p1q1);
            p1q1_output = vbslq_u16(needs_filter_mask_8, p1q1_output, p1q1);
            p0q0_output = vbslq_u16(is_flat4_mask_8, f8_p0q0, f4_p0q0);
            p0q0_output = vbslq_u16(needs_filter_mask_8, p0q0_output, p0q0);
        }
    }

    uint16x8_t output[4] = {p0q0_output, p1q1_output, p2q2_output, p3q3};
    // After transpose, |output| will contain rows of the form:
    // p0 p1 p2 p3 q0 q1 q2 q3
    transpose_array_inplace_u16_4x8(output);

    // Reverse p values to produce original order:
    // p3 p2 p1 p0 q0 q1 q2 q3
    store_u16_8x4(s - 4,
                  pitch,
                  reverse_low_half(output[0]),
                  reverse_low_half(output[1]),
                  reverse_low_half(output[2]),
                  reverse_low_half(output[3]));
}

static inline void filter14(const uint16x8_t p6q6, const uint16x8_t p5q5, const uint16x8_t p4q4, const uint16x8_t p3q3,
                            const uint16x8_t p2q2, const uint16x8_t p1q1, const uint16x8_t p0q0,
                            uint16x8_t *const p5q5_output, uint16x8_t *const p4q4_output, uint16x8_t *const p3q3_output,
                            uint16x8_t *const p2q2_output, uint16x8_t *const p1q1_output,
                            uint16x8_t *const p0q0_output) {
    // Sum p5 and q5 output from opposite directions.
    // p5 = (7 * p6) + (2 * p5) + (2 * p4) + p3 + p2 + p1 + p0 + q0
    //      ^^^^^^^^
    // q5 = p0 + q0 + q1 + q2 + q3 + (2 * q4) + (2 * q5) + (7 * q6)
    //                                                     ^^^^^^^^
    const uint16x8_t p6q6_x7 = vsubq_u16(vshlq_n_u16(p6q6, 3), p6q6);

    // p5 = (7 * p6) + (2 * p5) + (2 * p4) + p3 + p2 + p1 + p0 + q0
    //                 ^^^^^^^^^^^^^^^^^^^
    // q5 = p0 + q0 + q1 + q2 + q3 + (2 * q4) + (2 * q5) + (7 * q6)
    //                               ^^^^^^^^^^^^^^^^^^^
    const uint16x8_t p45q45 = vaddq_u16(p5q5, p4q4);
    uint16x8_t       sum    = vmlaq_n_u16(p6q6_x7, p45q45, 2);

    // p5 = (7 * p6) + (2 * p5) + (2 * p4) + p3 + p2 + p1 + p0 + q0
    //                                       ^^^^^^^
    // q5 = p0 + q0 + q1 + q2 + q3 + (2 * q4) + (2 * q5) + (7 * q6)
    //                     ^^^^^^^
    sum = vaddq_u16(vaddq_u16(p3q3, p2q2), sum);

    // p5 = (7 * p6) + (2 * p5) + (2 * p4) + p3 + p2 + p1 + p0 + q0
    //                                                 ^^^^^^^
    // q5 = p0 + q0 + q1 + q2 + q3 + (2 * q4) + (2 * q5) + (7 * q6)
    //           ^^^^^^^
    sum = vaddq_u16(vaddq_u16(p1q1, p0q0), sum);

    // p5 = (7 * p6) + (2 * p5) + (2 * p4) + p3 + p2 + p1 + p0 + q0
    //                                                           ^^
    // q5 = p0 + q0 + q1 + q2 + q3 + (2 * q4) + (2 * q5) + (7 * q6)
    //      ^^
    const uint16x8_t q0p0 = vextq_u16(p0q0, p0q0, 4);
    sum                   = vaddq_u16(sum, q0p0);

    *p5q5_output = vrshrq_n_u16(sum, 4);

    // Convert to p4 and q4 output:
    // p4 = p5 - (2 * p6) + p3 + q1
    // q4 = q5 - (2 * q6) + q3 + p1
    sum                   = vsubq_u16(sum, vshlq_n_u16(p6q6, 1));
    const uint16x8_t q1p1 = vextq_u16(p1q1, p1q1, 4);
    sum                   = vaddq_u16(vaddq_u16(p3q3, q1p1), sum);

    *p4q4_output = vrshrq_n_u16(sum, 4);

    // Convert to p3 and q3 output:
    // p3 = p4 - p6 - p5 + p2 + q2
    // q3 = q4 - q6 - q5 + q2 + p2
    sum                   = vsubq_u16(sum, vaddq_u16(p6q6, p5q5));
    const uint16x8_t q2p2 = vextq_u16(p2q2, p2q2, 4);
    sum                   = vaddq_u16(vaddq_u16(p2q2, q2p2), sum);

    *p3q3_output = vrshrq_n_u16(sum, 4);

    // Convert to p2 and q2 output:
    // p2 = p3 - p6 - p4 + p1 + q3
    // q2 = q3 - q6 - q4 + q1 + p3
    sum                   = vsubq_u16(sum, vaddq_u16(p6q6, p4q4));
    const uint16x8_t q3p3 = vextq_u16(p3q3, p3q3, 4);
    sum                   = vaddq_u16(vaddq_u16(p1q1, q3p3), sum);

    *p2q2_output = vrshrq_n_u16(sum, 4);

    // Convert to p1 and q1 output:
    // p1 = p2 - p6 - p3 + p0 + q4
    // q1 = q2 - q6 - q3 + q0 + p4
    sum                   = vsubq_u16(sum, vaddq_u16(p6q6, p3q3));
    const uint16x8_t q4p4 = vextq_u16(p4q4, p4q4, 4);
    sum                   = vaddq_u16(vaddq_u16(p0q0, q4p4), sum);

    *p1q1_output = vrshrq_n_u16(sum, 4);

    // Convert to p0 and q0 output:
    // p0 = p1 - p6 - p2 + q0 + q5
    // q0 = q1 - q6 - q2 + p0 + p5
    sum                   = vsubq_u16(sum, vaddq_u16(p6q6, p2q2));
    const uint16x8_t q5p5 = vextq_u16(p5q5, p5q5, 4);
    sum                   = vaddq_u16(vaddq_u16(q0p0, q5p5), sum);

    *p0q0_output = vrshrq_n_u16(sum, 4);
}

void svt_aom_highbd_lpf_horizontal_14_neon(uint16_t *s, int pitch, const uint8_t *blimit, const uint8_t *limit,
                                           const uint8_t *thresh, int bd) {
    uint16x4_t src[14];
    load_u16_4x14(s - 7 * pitch,
                  pitch,
                  &src[0],
                  &src[1],
                  &src[2],
                  &src[3],
                  &src[4],
                  &src[5],
                  &src[6],
                  &src[7],
                  &src[8],
                  &src[9],
                  &src[10],
                  &src[11],
                  &src[12],
                  &src[13]);

    // Adjust thresholds to bitdepth.
    const int        outer_thresh = *blimit << (bd - 8);
    const int        inner_thresh = *limit << (bd - 8);
    const int        hev_thresh   = *thresh << (bd - 8);
    const uint16x4_t outer_mask   = outer_threshold(src[5], src[6], src[7], src[8], outer_thresh);
    uint16x4_t       hev_mask;
    uint16x4_t       needs_filter_mask;
    uint16x4_t       is_flat4_mask;
    const uint16x8_t p0q0 = vcombine_u16(src[6], src[7]);
    const uint16x8_t p1q1 = vcombine_u16(src[5], src[8]);
    const uint16x8_t p2q2 = vcombine_u16(src[4], src[9]);
    const uint16x8_t p3q3 = vcombine_u16(src[3], src[10]);
    filter8_masks(p3q3,
                  p2q2,
                  p1q1,
                  p0q0,
                  hev_thresh,
                  outer_mask,
                  inner_thresh,
                  bd,
                  &needs_filter_mask,
                  &is_flat4_mask,
                  &hev_mask);

    if (vget_lane_u64(vreinterpret_u64_u16(needs_filter_mask), 0) == 0) {
        // None of the values will be filtered.
        return;
    }
    const uint16x8_t p4q4 = vcombine_u16(src[2], src[11]);
    const uint16x8_t p5q5 = vcombine_u16(src[1], src[12]);
    const uint16x8_t p6q6 = vcombine_u16(src[0], src[13]);
    // Mask to choose between the outputs of filter8 and filter14.
    // As with the derivation of |is_flat4_mask|, the question of whether to use
    // filter14 is only raised where |is_flat4_mask| is true.
    const uint16x4_t is_flat4_outer_mask = vand_u16(
        is_flat4_mask, is_flat4(vabdq_u16(p0q0, p4q4), vabdq_u16(p0q0, p5q5), vabdq_u16(p0q0, p6q6), bd));

    uint16x8_t p0q0_output, p1q1_output, p2q2_output, p3q3_output, p4q4_output, p5q5_output;
    uint16x8_t f8_p2q2, f8_p1q1, f8_p0q0;
    uint16x8_t f14_p5q5, f14_p4q4, f14_p3q3, f14_p2q2, f14_p1q1, f14_p0q0;
    if (vget_lane_s64(vreinterpret_s64_u16(is_flat4_outer_mask), 0) == -1) {
        // filter14() applies to all values.
        filter14(
            p6q6, p5q5, p4q4, p3q3, p2q2, p1q1, p0q0, &f14_p5q5, &f14_p4q4, &f14_p3q3, &f14_p2q2, &f14_p1q1, &f14_p0q0);
        p5q5_output = f14_p5q5;
        p4q4_output = f14_p4q4;
        p3q3_output = f14_p3q3;
        p2q2_output = f14_p2q2;
        p1q1_output = f14_p1q1;
        p0q0_output = f14_p0q0;
    } else if (vget_lane_s64(vreinterpret_s64_u16(is_flat4_mask), 0) == -1 &&
               vget_lane_u64(vreinterpret_u64_u16(is_flat4_outer_mask), 0) == 0) {
        // filter8() applies to all values.
        filter8(p3q3, p2q2, p1q1, p0q0, &f8_p2q2, &f8_p1q1, &f8_p0q0);
        p5q5_output = p5q5;
        p4q4_output = p4q4;
        p3q3_output = p3q3;
        p2q2_output = f8_p2q2;
        p1q1_output = f8_p1q1;
        p0q0_output = f8_p0q0;
    } else {
        // Copy the masks to the high bits for packed comparisons later.
        const uint16x8_t hev_mask_8          = vcombine_u16(hev_mask, hev_mask);
        const uint16x8_t needs_filter_mask_8 = vcombine_u16(needs_filter_mask, needs_filter_mask);

        uint16x8_t       f4_p1q1;
        uint16x8_t       f4_p0q0;
        const uint16x8_t p0q1 = vcombine_u16(src[6], src[8]);
        filter4(p0q0, p0q1, p1q1, hev_mask, bd, &f4_p1q1, &f4_p0q0);
        f4_p1q1 = vbslq_u16(hev_mask_8, p1q1, f4_p1q1);
        // Because we did not return after testing |needs_filter_mask| we know it is
        // nonzero. |is_flat4_mask| controls whether the needed filter is filter4 or
        // filter8. Therefore if it is false when |needs_filter_mask| is true, filter8
        // output is not used.
        const uint64x1_t need_filter8 = vreinterpret_u64_u16(is_flat4_mask);
        if (vget_lane_u64(need_filter8, 0) == 0) {
            // filter8() and filter14() do not apply, but filter4() applies to one or
            // more values.
            p5q5_output = p5q5;
            p4q4_output = p4q4;
            p3q3_output = p3q3;
            p2q2_output = p2q2;
            p1q1_output = vbslq_u16(needs_filter_mask_8, f4_p1q1, p1q1);
            p0q0_output = vbslq_u16(needs_filter_mask_8, f4_p0q0, p0q0);
        } else {
            const uint16x8_t use_filter8_mask = vcombine_u16(is_flat4_mask, is_flat4_mask);
            filter8(p3q3, p2q2, p1q1, p0q0, &f8_p2q2, &f8_p1q1, &f8_p0q0);
            const uint64x1_t need_filter14 = vreinterpret_u64_u16(is_flat4_outer_mask);
            if (vget_lane_u64(need_filter14, 0) == 0) {
                // filter14() does not apply, but filter8() and filter4() apply to one or
                // more values.
                p5q5_output = p5q5;
                p4q4_output = p4q4;
                p3q3_output = p3q3;
                p2q2_output = vbslq_u16(use_filter8_mask, f8_p2q2, p2q2);
                p1q1_output = vbslq_u16(use_filter8_mask, f8_p1q1, f4_p1q1);
                p1q1_output = vbslq_u16(needs_filter_mask_8, p1q1_output, p1q1);
                p0q0_output = vbslq_u16(use_filter8_mask, f8_p0q0, f4_p0q0);
                p0q0_output = vbslq_u16(needs_filter_mask_8, p0q0_output, p0q0);
            } else {
                // All filters may contribute values to final outputs.
                const uint16x8_t use_filter14_mask = vcombine_u16(is_flat4_outer_mask, is_flat4_outer_mask);
                filter14(p6q6,
                         p5q5,
                         p4q4,
                         p3q3,
                         p2q2,
                         p1q1,
                         p0q0,
                         &f14_p5q5,
                         &f14_p4q4,
                         &f14_p3q3,
                         &f14_p2q2,
                         &f14_p1q1,
                         &f14_p0q0);
                p5q5_output = vbslq_u16(use_filter14_mask, f14_p5q5, p5q5);
                p4q4_output = vbslq_u16(use_filter14_mask, f14_p4q4, p4q4);
                p3q3_output = vbslq_u16(use_filter14_mask, f14_p3q3, p3q3);
                p2q2_output = vbslq_u16(use_filter14_mask, f14_p2q2, f8_p2q2);
                p2q2_output = vbslq_u16(use_filter8_mask, p2q2_output, p2q2);
                p2q2_output = vbslq_u16(needs_filter_mask_8, p2q2_output, p2q2);
                p1q1_output = vbslq_u16(use_filter14_mask, f14_p1q1, f8_p1q1);
                p1q1_output = vbslq_u16(use_filter8_mask, p1q1_output, f4_p1q1);
                p1q1_output = vbslq_u16(needs_filter_mask_8, p1q1_output, p1q1);
                p0q0_output = vbslq_u16(use_filter14_mask, f14_p0q0, f8_p0q0);
                p0q0_output = vbslq_u16(use_filter8_mask, p0q0_output, f4_p0q0);
                p0q0_output = vbslq_u16(needs_filter_mask_8, p0q0_output, p0q0);
            }
        }
    }

    store_u16_4x12(s - 6 * pitch,
                   pitch,
                   vget_low_u16(p5q5_output),
                   vget_low_u16(p4q4_output),
                   vget_low_u16(p3q3_output),
                   vget_low_u16(p2q2_output),
                   vget_low_u16(p1q1_output),
                   vget_low_u16(p0q0_output),
                   vget_high_u16(p0q0_output),
                   vget_high_u16(p1q1_output),
                   vget_high_u16(p2q2_output),
                   vget_high_u16(p3q3_output),
                   vget_high_u16(p4q4_output),
                   vget_high_u16(p5q5_output));
}

static inline uint16x8x2_t permute_acdb64(const uint16x8_t ab, const uint16x8_t cd) {
    uint16x8x2_t acdb;
    // ab cd -> ac
    acdb.val[0] = vreinterpretq_u16_u64(vtrn1q_u64(vreinterpretq_u64_u16(ab), vreinterpretq_u64_u16(cd)));
    // ab cd -> db
    acdb.val[1] = vreinterpretq_u16_u64(vtrn2q_u64(vreinterpretq_u64_u16(cd), vreinterpretq_u64_u16(ab)));
    return acdb;
}

void svt_aom_highbd_lpf_vertical_14_neon(uint16_t *s, int pitch, const uint8_t *blimit, const uint8_t *limit,
                                         const uint8_t *thresh, int bd) {
    // Low halves:  p7 p6 p5 p4
    // High halves: p3 p2 p1 p0
    uint16x8_t src_p[4];
    load_u16_8x4(s - 8, pitch, &src_p[0], &src_p[1], &src_p[2], &src_p[3]);
    // p7 will be the low half of src_p[0]. Not used until the end.
    transpose_array_inplace_u16_4x8(src_p);

    // Low halves:  q0 q1 q2 q3
    // High halves: q4 q5 q6 q7
    uint16x8_t src_q[4];
    load_u16_8x4(s, pitch, &src_q[0], &src_q[1], &src_q[2], &src_q[3]);
    // q7 will be the high half of src_q[3]. Not used until the end.
    transpose_array_inplace_u16_4x8(src_q);

    // Adjust thresholds to bitdepth.
    const int        outer_thresh = *blimit << (bd - 8);
    const int        inner_thresh = *limit << (bd - 8);
    const int        hev_thresh   = *thresh << (bd - 8);
    const uint16x4_t outer_mask   = outer_threshold(
        vget_high_u16(src_p[2]), vget_high_u16(src_p[3]), vget_low_u16(src_q[0]), vget_low_u16(src_q[1]), outer_thresh);
    const uint16x8_t p0q0 = vextq_u16(src_p[3], src_q[0], 4);
    const uint16x8_t p1q1 = vextq_u16(src_p[2], src_q[1], 4);
    const uint16x8_t p2q2 = vextq_u16(src_p[1], src_q[2], 4);
    const uint16x8_t p3q3 = vextq_u16(src_p[0], src_q[3], 4);
    uint16x4_t       hev_mask;
    uint16x4_t       needs_filter_mask;
    uint16x4_t       is_flat4_mask;
    filter8_masks(p3q3,
                  p2q2,
                  p1q1,
                  p0q0,
                  hev_thresh,
                  outer_mask,
                  inner_thresh,
                  bd,
                  &needs_filter_mask,
                  &is_flat4_mask,
                  &hev_mask);

    if (vget_lane_u64(vreinterpret_u64_u16(needs_filter_mask), 0) == 0) {
        // None of the values will be filtered.
        return;
    }

    const uint16x8_t p4q4 = vcombine_u16(vget_low_u16(src_p[3]), vget_high_u16(src_q[0]));
    const uint16x8_t p5q5 = vcombine_u16(vget_low_u16(src_p[2]), vget_high_u16(src_q[1]));
    const uint16x8_t p6q6 = vcombine_u16(vget_low_u16(src_p[1]), vget_high_u16(src_q[2]));
    const uint16x8_t p7q7 = vcombine_u16(vget_low_u16(src_p[0]), vget_high_u16(src_q[3]));
    // Mask to choose between the outputs of filter8 and filter14.
    // As with the derivation of |is_flat4_mask|, the question of whether to use
    // filter14 is only raised where |is_flat4_mask| is true.
    const uint16x4_t is_flat4_outer_mask = vand_u16(
        is_flat4_mask, is_flat4(vabdq_u16(p0q0, p4q4), vabdq_u16(p0q0, p5q5), vabdq_u16(p0q0, p6q6), bd));

    uint16x8_t p0q0_output, p1q1_output, p2q2_output, p3q3_output, p4q4_output, p5q5_output;
    uint16x8_t f8_p2q2, f8_p1q1, f8_p0q0;
    uint16x8_t f14_p5q5, f14_p4q4, f14_p3q3, f14_p2q2, f14_p1q1, f14_p0q0;
    if (vget_lane_s64(vreinterpret_s64_u16(is_flat4_outer_mask), 0) == -1) {
        // filter14() applies to all values.
        filter14(
            p6q6, p5q5, p4q4, p3q3, p2q2, p1q1, p0q0, &f14_p5q5, &f14_p4q4, &f14_p3q3, &f14_p2q2, &f14_p1q1, &f14_p0q0);
        p5q5_output = f14_p5q5;
        p4q4_output = f14_p4q4;
        p3q3_output = f14_p3q3;
        p2q2_output = f14_p2q2;
        p1q1_output = f14_p1q1;
        p0q0_output = f14_p0q0;
    } else if (vget_lane_s64(vreinterpret_s64_u16(is_flat4_mask), 0) == -1 &&
               vget_lane_u64(vreinterpret_u64_u16(is_flat4_outer_mask), 0) == 0) {
        // filter8() applies to all values.
        filter8(p3q3, p2q2, p1q1, p0q0, &f8_p2q2, &f8_p1q1, &f8_p0q0);
        p5q5_output = p5q5;
        p4q4_output = p4q4;
        p3q3_output = p3q3;
        p2q2_output = f8_p2q2;
        p1q1_output = f8_p1q1;
        p0q0_output = f8_p0q0;
    } else {
        // Copy the masks to the high bits for packed comparisons later.
        const uint16x8_t hev_mask_8          = vcombine_u16(hev_mask, hev_mask);
        const uint16x8_t needs_filter_mask_8 = vcombine_u16(needs_filter_mask, needs_filter_mask);

        uint16x8_t       f4_p1q1;
        uint16x8_t       f4_p0q0;
        const uint16x8_t p0q1 = vcombine_u16(vget_low_u16(p0q0), vget_high_u16(p1q1));
        filter4(p0q0, p0q1, p1q1, hev_mask, bd, &f4_p1q1, &f4_p0q0);
        f4_p1q1 = vbslq_u16(hev_mask_8, p1q1, f4_p1q1);
        // Because we did not return after testing |needs_filter_mask| we know it is
        // nonzero. |is_flat4_mask| controls whether the needed filter is filter4 or
        // filter8. Therefore if it is false when |needs_filter_mask| is true, filter8
        // output is not used.
        const uint64x1_t need_filter8 = vreinterpret_u64_u16(is_flat4_mask);
        if (vget_lane_u64(need_filter8, 0) == 0) {
            // filter8() and filter14() do not apply, but filter4() applies to one or
            // more values.
            p5q5_output = p5q5;
            p4q4_output = p4q4;
            p3q3_output = p3q3;
            p2q2_output = p2q2;
            p1q1_output = vbslq_u16(needs_filter_mask_8, f4_p1q1, p1q1);
            p0q0_output = vbslq_u16(needs_filter_mask_8, f4_p0q0, p0q0);
        } else {
            const uint16x8_t use_filter8_mask = vcombine_u16(is_flat4_mask, is_flat4_mask);
            filter8(p3q3, p2q2, p1q1, p0q0, &f8_p2q2, &f8_p1q1, &f8_p0q0);
            const uint64x1_t need_filter14 = vreinterpret_u64_u16(is_flat4_outer_mask);
            if (vget_lane_u64(need_filter14, 0) == 0) {
                // filter14() does not apply, but filter8() and filter4() apply to one or
                // more values.
                p5q5_output = p5q5;
                p4q4_output = p4q4;
                p3q3_output = p3q3;
                p2q2_output = vbslq_u16(use_filter8_mask, f8_p2q2, p2q2);
                p1q1_output = vbslq_u16(use_filter8_mask, f8_p1q1, f4_p1q1);
                p1q1_output = vbslq_u16(needs_filter_mask_8, p1q1_output, p1q1);
                p0q0_output = vbslq_u16(use_filter8_mask, f8_p0q0, f4_p0q0);
                p0q0_output = vbslq_u16(needs_filter_mask_8, p0q0_output, p0q0);
            } else {
                // All filters may contribute values to final outputs.
                const uint16x8_t use_filter14_mask = vcombine_u16(is_flat4_outer_mask, is_flat4_outer_mask);
                filter14(p6q6,
                         p5q5,
                         p4q4,
                         p3q3,
                         p2q2,
                         p1q1,
                         p0q0,
                         &f14_p5q5,
                         &f14_p4q4,
                         &f14_p3q3,
                         &f14_p2q2,
                         &f14_p1q1,
                         &f14_p0q0);
                p5q5_output = vbslq_u16(use_filter14_mask, f14_p5q5, p5q5);
                p4q4_output = vbslq_u16(use_filter14_mask, f14_p4q4, p4q4);
                p3q3_output = vbslq_u16(use_filter14_mask, f14_p3q3, p3q3);
                p2q2_output = vbslq_u16(use_filter14_mask, f14_p2q2, f8_p2q2);
                p2q2_output = vbslq_u16(use_filter8_mask, p2q2_output, p2q2);
                p2q2_output = vbslq_u16(needs_filter_mask_8, p2q2_output, p2q2);
                p1q1_output = vbslq_u16(use_filter14_mask, f14_p1q1, f8_p1q1);
                p1q1_output = vbslq_u16(use_filter8_mask, p1q1_output, f4_p1q1);
                p1q1_output = vbslq_u16(needs_filter_mask_8, p1q1_output, p1q1);
                p0q0_output = vbslq_u16(use_filter14_mask, f14_p0q0, f8_p0q0);
                p0q0_output = vbslq_u16(use_filter8_mask, p0q0_output, f4_p0q0);
                p0q0_output = vbslq_u16(needs_filter_mask_8, p0q0_output, p0q0);
            }
        }
    }

    // To get the correctly ordered rows from the transpose, we need:
    // p7p3 p6p2 p5p1 p4p0
    // q0q4 q1q5 q2q6 q3q7
    const uint16x8x2_t p7p3_q3q7 = permute_acdb64(p7q7, p3q3_output);
    const uint16x8x2_t p6p2_q2q6 = permute_acdb64(p6q6, p2q2_output);
    const uint16x8x2_t p5p1_q1q5 = permute_acdb64(p5q5_output, p1q1_output);
    const uint16x8x2_t p4p0_q0q4 = permute_acdb64(p4q4_output, p0q0_output);

    uint16x8_t output_p[4] = {p7p3_q3q7.val[0], p6p2_q2q6.val[0], p5p1_q1q5.val[0], p4p0_q0q4.val[0]};
    uint16x8_t output_q[4] = {p4p0_q0q4.val[1], p5p1_q1q5.val[1], p6p2_q2q6.val[1], p7p3_q3q7.val[1]};

    transpose_array_inplace_u16_4x8(output_p);
    transpose_array_inplace_u16_4x8(output_q);

    // Reverse p values to produce original order:
    // p3 p2 p1 p0 q0 q1 q2 q3
    store_u16_8x4(s - 8, pitch, output_p[0], output_p[1], output_p[2], output_p[3]);
    store_u16_8x4(s, pitch, output_q[0], output_q[1], output_q[2], output_q[3]);
}
