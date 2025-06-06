/*
* Copyright (c) 2023, Alliance for Open Media. All rights reserved
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at www.aomedia.org/license/software. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at www.aomedia.org/license/patent.
*/

#include <arm_neon.h>

#include "mem_neon.h"
#include "sum_neon.h"
#include "transpose_neon.h"

static inline void sse_16x1_neon(const uint8_t *src, const uint8_t *ref, uint32x4_t *sse) {
    uint8x16_t s = vld1q_u8(src);
    uint8x16_t r = vld1q_u8(ref);

    uint8x16_t abs_diff    = vabdq_u8(s, r);
    uint8x8_t  abs_diff_lo = vget_low_u8(abs_diff);
    uint8x8_t  abs_diff_hi = vget_high_u8(abs_diff);

    *sse = vpadalq_u16(*sse, vmull_u8(abs_diff_lo, abs_diff_lo));
    *sse = vpadalq_u16(*sse, vmull_u8(abs_diff_hi, abs_diff_hi));
}

static inline void sse_8x1_neon(const uint8_t *src, const uint8_t *ref, uint32x4_t *sse) {
    uint8x8_t s = vld1_u8(src);
    uint8x8_t r = vld1_u8(ref);

    uint8x8_t abs_diff = vabd_u8(s, r);

    *sse = vpadalq_u16(*sse, vmull_u8(abs_diff, abs_diff));
}

static inline void sse_4x2_neon(const uint8_t *src, int src_stride, const uint8_t *ref, int ref_stride,
                                uint32x4_t *sse) {
    uint8x8_t s = load_u8_4x2(src, src_stride);
    uint8x8_t r = load_u8_4x2(ref, ref_stride);

    uint8x8_t abs_diff = vabd_u8(s, r);

    *sse = vpadalq_u16(*sse, vmull_u8(abs_diff, abs_diff));
}

static inline uint32_t sse_8xh_neon(const uint8_t *src, int src_stride, const uint8_t *ref, int ref_stride,
                                    int height) {
    uint32x4_t sse = vdupq_n_u32(0);

    int i = height;
    do {
        sse_8x1_neon(src, ref, &sse);

        src += src_stride;
        ref += ref_stride;
    } while (--i != 0);

    return vaddvq_u32(sse);
}

static inline uint32_t sse_4xh_neon(const uint8_t *src, int src_stride, const uint8_t *ref, int ref_stride,
                                    int height) {
    uint32x4_t sse = vdupq_n_u32(0);

    int i = height;
    do {
        sse_4x2_neon(src, src_stride, ref, ref_stride, &sse);

        src += 2 * src_stride;
        ref += 2 * ref_stride;
        i -= 2;
    } while (i != 0);

    return vaddvq_u32(sse);
}

static inline uint32_t sse_wxh_neon(const uint8_t *src, int src_stride, const uint8_t *ref, int ref_stride, int width,
                                    int height) {
    uint32x4_t sse = vdupq_n_u32(0);
    int        i, j;
    if ((width & 0x07) && ((width & 0x07) < 5)) {
        i = height;
        do {
            j = 0;
            do {
                sse_8x1_neon(src + j, ref + j, &sse);
                sse_8x1_neon(src + j + src_stride, ref + j + ref_stride, &sse);
                j += 8;
            } while (j + 4 < width);

            sse_4x2_neon(src + j, src_stride, ref + j, ref_stride, &sse);
            src += 2 * src_stride;
            ref += 2 * ref_stride;
            i -= 2;
        } while (i != 0);
    } else {
        i = height;
        do {
            j = 0;
            do {
                sse_8x1_neon(src + j, ref + j, &sse);
                j += 8;
            } while (j < width);

            src += src_stride;
            ref += ref_stride;
        } while (--i != 0);
    }
    return vaddvq_u32(sse);
}

static inline uint32_t sse_128xh_neon(const uint8_t *src, int src_stride, const uint8_t *ref, int ref_stride,
                                      int height) {
    uint32x4_t sse[2] = {vdupq_n_u32(0), vdupq_n_u32(0)};

    int i = height;
    do {
        sse_16x1_neon(src, ref, &sse[0]);
        sse_16x1_neon(src + 16, ref + 16, &sse[1]);
        sse_16x1_neon(src + 32, ref + 32, &sse[0]);
        sse_16x1_neon(src + 48, ref + 48, &sse[1]);
        sse_16x1_neon(src + 64, ref + 64, &sse[0]);
        sse_16x1_neon(src + 80, ref + 80, &sse[1]);
        sse_16x1_neon(src + 96, ref + 96, &sse[0]);
        sse_16x1_neon(src + 112, ref + 112, &sse[1]);

        src += src_stride;
        ref += ref_stride;
    } while (--i != 0);

    return vaddvq_u32(vaddq_u32(sse[0], sse[1]));
}

static inline uint32_t sse_64xh_neon(const uint8_t *src, int src_stride, const uint8_t *ref, int ref_stride,
                                     int height) {
    uint32x4_t sse[2] = {vdupq_n_u32(0), vdupq_n_u32(0)};

    int i = height;
    do {
        sse_16x1_neon(src, ref, &sse[0]);
        sse_16x1_neon(src + 16, ref + 16, &sse[1]);
        sse_16x1_neon(src + 32, ref + 32, &sse[0]);
        sse_16x1_neon(src + 48, ref + 48, &sse[1]);

        src += src_stride;
        ref += ref_stride;
    } while (--i != 0);

    return vaddvq_u32(vaddq_u32(sse[0], sse[1]));
}

static inline uint32_t sse_32xh_neon(const uint8_t *src, int src_stride, const uint8_t *ref, int ref_stride,
                                     int height) {
    uint32x4_t sse[2] = {vdupq_n_u32(0), vdupq_n_u32(0)};

    int i = height;
    do {
        sse_16x1_neon(src, ref, &sse[0]);
        sse_16x1_neon(src + 16, ref + 16, &sse[1]);

        src += src_stride;
        ref += ref_stride;
    } while (--i != 0);

    return vaddvq_u32(vaddq_u32(sse[0], sse[1]));
}

static inline uint32_t sse_16xh_neon(const uint8_t *src, int src_stride, const uint8_t *ref, int ref_stride,
                                     int height) {
    uint32x4_t sse[2] = {vdupq_n_u32(0), vdupq_n_u32(0)};

    int i = height;
    do {
        sse_16x1_neon(src, ref, &sse[0]);
        src += src_stride;
        ref += ref_stride;
        sse_16x1_neon(src, ref, &sse[1]);
        src += src_stride;
        ref += ref_stride;
        i -= 2;
    } while (i != 0);

    return vaddvq_u32(vaddq_u32(sse[0], sse[1]));
}

int64_t svt_aom_sse_neon(const uint8_t *src, int src_stride, const uint8_t *ref, int ref_stride, int width,
                         int height) {
    switch (width) {
    case 4: {
        return sse_4xh_neon(src, src_stride, ref, ref_stride, height);
    }
    case 8: {
        return sse_8xh_neon(src, src_stride, ref, ref_stride, height);
    }
    case 16: {
        return sse_16xh_neon(src, src_stride, ref, ref_stride, height);
    }
    case 32: {
        return sse_32xh_neon(src, src_stride, ref, ref_stride, height);
    }
    case 64: {
        return sse_64xh_neon(src, src_stride, ref, ref_stride, height);
    }
    case 128: {
        return sse_128xh_neon(src, src_stride, ref, ref_stride, height);
    }
    default: {
        return sse_wxh_neon(src, src_stride, ref, ref_stride, width, height);
    }
    }
}

uint64_t svt_spatial_full_distortion_kernel_neon(uint8_t *src, uint32_t src_offset, uint32_t src_stride, uint8_t *ref,
                                                 uint32_t ref_offset, uint32_t ref_stride, uint32_t width,
                                                 uint32_t height) {
    src += src_offset;
    ref += ref_offset;
    switch (width) {
    case 4: return sse_4xh_neon(src, src_stride, ref, ref_stride, height);
    case 8: return sse_8xh_neon(src, src_stride, ref, ref_stride, height);
    case 16: return sse_16xh_neon(src, src_stride, ref, ref_stride, height);
    case 32: return sse_32xh_neon(src, src_stride, ref, ref_stride, height);
    case 64: return sse_64xh_neon(src, src_stride, ref, ref_stride, height);
    case 128: return sse_128xh_neon(src, src_stride, ref, ref_stride, height);
    default: return sse_wxh_neon(src, src_stride, ref, ref_stride, width, height);
    }
}
