/*
 * Copyright (c) 2017, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
 */

#include <immintrin.h>
#include <assert.h>

#include "definitions.h"
#include "common_dsp_rtcd.h"

#include "convolve_avx2.h"
#include "synonyms.h"
#include "synonyms_avx2.h"
#include "convolve.h"

void svt_av1_highbd_convolve_2d_sr_avx2(const uint16_t *src, int32_t src_stride, uint16_t *dst, int32_t dst_stride,
                                        int32_t w, int32_t h, const InterpFilterParams *filter_params_x,
                                        const InterpFilterParams *filter_params_y, const int32_t subpel_x_q4,
                                        const int32_t subpel_y_q4, ConvolveParams *conv_params, int32_t bd) {
    DECLARE_ALIGNED(32, int16_t, im_block[(MAX_SB_SIZE + MAX_FILTER_TAP) * 8]);
    int32_t               im_h      = h + filter_params_y->taps - 1;
    int32_t               im_stride = 8;
    int32_t               i, j;
    const int32_t         fo_vert  = filter_params_y->taps / 2 - 1;
    const int32_t         fo_horiz = filter_params_x->taps / 2 - 1;
    const uint16_t *const src_ptr  = src - fo_vert * src_stride - fo_horiz;

    // Check that, even with 12-bit input, the intermediate values will fit
    // into an unsigned 16-bit intermediate array.
    assert(bd + FILTER_BITS + 2 - conv_params->round_0 <= 16);

    __m256i s[8], coeffs_y[4], coeffs_x[4];

    const __m256i round_const_x = _mm256_set1_epi32(((1 << conv_params->round_0) >> 1) + (1 << (bd + FILTER_BITS - 1)));
    const __m128i round_shift_x = _mm_cvtsi32_si128(conv_params->round_0);

    const __m256i round_const_y = _mm256_set1_epi32(((1 << conv_params->round_1) >> 1) -
                                                    (1 << (bd + 2 * FILTER_BITS - conv_params->round_0 - 1)));
    const __m128i round_shift_y = _mm_cvtsi32_si128(conv_params->round_1);

    const int32_t bits             = FILTER_BITS * 2 - conv_params->round_0 - conv_params->round_1;
    const __m128i round_shift_bits = _mm_cvtsi32_si128(bits);
    const __m256i round_const_bits = _mm256_set1_epi32((1 << bits) >> 1);
    const __m256i clp_pxl          = _mm256_set1_epi16(bd == 10 ? 1023 : (bd == 12 ? 4095 : 255));
    const __m256i zero             = _mm256_setzero_si256();

    prepare_coeffs_8tap_avx2(filter_params_x, subpel_x_q4, coeffs_x);
    prepare_coeffs_8tap_avx2(filter_params_y, subpel_y_q4, coeffs_y);

    for (j = 0; j < w; j += 8) {
        /* Horizontal filter */
        {
            for (i = 0; i < im_h; i += 2) {
                const __m256i row0 = _mm256_loadu_si256((__m256i *)&src_ptr[i * src_stride + j]);
                __m256i       row1 = _mm256_set1_epi16(0);
                if (i + 1 < im_h)
                    row1 = _mm256_loadu_si256((__m256i *)&src_ptr[(i + 1) * src_stride + j]);

                const __m256i r0 = yy_unpacklo_epi128(row0, row1);
                const __m256i r1 = yy_unpackhi_epi128(row0, row1);

                // even pixels
                s[0] = _mm256_alignr_epi8(r1, r0, 0);
                s[1] = _mm256_alignr_epi8(r1, r0, 4);
                s[2] = _mm256_alignr_epi8(r1, r0, 8);
                s[3] = _mm256_alignr_epi8(r1, r0, 12);

                __m256i res_even = convolve16_8tap_avx2(s, coeffs_x);
                res_even         = _mm256_sra_epi32(_mm256_add_epi32(res_even, round_const_x), round_shift_x);

                // odd pixels
                s[0] = _mm256_alignr_epi8(r1, r0, 2);
                s[1] = _mm256_alignr_epi8(r1, r0, 6);
                s[2] = _mm256_alignr_epi8(r1, r0, 10);
                s[3] = _mm256_alignr_epi8(r1, r0, 14);

                __m256i res_odd = convolve16_8tap_avx2(s, coeffs_x);
                res_odd         = _mm256_sra_epi32(_mm256_add_epi32(res_odd, round_const_x), round_shift_x);

                __m256i res_even1 = _mm256_packs_epi32(res_even, res_even);
                __m256i res_odd1  = _mm256_packs_epi32(res_odd, res_odd);
                __m256i res       = _mm256_unpacklo_epi16(res_even1, res_odd1);

                _mm256_storeu_si256((__m256i *)&im_block[i * im_stride], res);
            }
        }

        /* Vertical filter */
        {
            __m256i s0 = _mm256_loadu_si256((__m256i *)(im_block + 0 * im_stride));
            __m256i s1 = _mm256_loadu_si256((__m256i *)(im_block + 1 * im_stride));
            __m256i s2 = _mm256_loadu_si256((__m256i *)(im_block + 2 * im_stride));
            __m256i s3 = _mm256_loadu_si256((__m256i *)(im_block + 3 * im_stride));
            __m256i s4 = _mm256_loadu_si256((__m256i *)(im_block + 4 * im_stride));
            __m256i s5 = _mm256_loadu_si256((__m256i *)(im_block + 5 * im_stride));

            s[0] = _mm256_unpacklo_epi16(s0, s1);
            s[1] = _mm256_unpacklo_epi16(s2, s3);
            s[2] = _mm256_unpacklo_epi16(s4, s5);

            s[4] = _mm256_unpackhi_epi16(s0, s1);
            s[5] = _mm256_unpackhi_epi16(s2, s3);
            s[6] = _mm256_unpackhi_epi16(s4, s5);

            for (i = 0; i < h; i += 2) {
                const int16_t *data = &im_block[i * im_stride];

                const __m256i s6 = _mm256_loadu_si256((__m256i *)(data + 6 * im_stride));
                const __m256i s7 = _mm256_loadu_si256((__m256i *)(data + 7 * im_stride));

                s[3] = _mm256_unpacklo_epi16(s6, s7);
                s[7] = _mm256_unpackhi_epi16(s6, s7);

                const __m256i res_a       = convolve16_8tap_avx2(s, coeffs_y);
                __m256i       res_a_round = _mm256_sra_epi32(_mm256_add_epi32(res_a, round_const_y), round_shift_y);

                res_a_round = _mm256_sra_epi32(_mm256_add_epi32(res_a_round, round_const_bits), round_shift_bits);

                if (w - j > 4) {
                    const __m256i res_b       = convolve16_8tap_avx2(s + 4, coeffs_y);
                    __m256i       res_b_round = _mm256_sra_epi32(_mm256_add_epi32(res_b, round_const_y), round_shift_y);
                    res_b_round = _mm256_sra_epi32(_mm256_add_epi32(res_b_round, round_const_bits), round_shift_bits);

                    __m256i res_16bit = _mm256_packs_epi32(res_a_round, res_b_round);
                    res_16bit         = _mm256_min_epi16(res_16bit, clp_pxl);
                    res_16bit         = _mm256_max_epi16(res_16bit, zero);

                    _mm_storeu_si128((__m128i *)&dst[i * dst_stride + j], _mm256_castsi256_si128(res_16bit));
                    _mm_storeu_si128((__m128i *)&dst[i * dst_stride + j + dst_stride],
                                     _mm256_extracti128_si256(res_16bit, 1));
                } else if (w == 4) {
                    res_a_round = _mm256_packs_epi32(res_a_round, res_a_round);
                    res_a_round = _mm256_min_epi16(res_a_round, clp_pxl);
                    res_a_round = _mm256_max_epi16(res_a_round, zero);

                    _mm_storel_epi64((__m128i *)&dst[i * dst_stride + j], _mm256_castsi256_si128(res_a_round));
                    _mm_storel_epi64((__m128i *)&dst[i * dst_stride + j + dst_stride],
                                     _mm256_extracti128_si256(res_a_round, 1));
                } else {
                    res_a_round = _mm256_packs_epi32(res_a_round, res_a_round);
                    res_a_round = _mm256_min_epi16(res_a_round, clp_pxl);
                    res_a_round = _mm256_max_epi16(res_a_round, zero);

                    xx_storel_32((__m128i *)&dst[i * dst_stride + j], _mm256_castsi256_si128(res_a_round));
                    xx_storel_32((__m128i *)&dst[i * dst_stride + j + dst_stride],
                                 _mm256_extracti128_si256(res_a_round, 1));
                }

                s[0] = s[1];
                s[1] = s[2];
                s[2] = s[3];

                s[4] = s[5];
                s[5] = s[6];
                s[6] = s[7];
            }
        }
    }
}

static INLINE void copy_64(const uint16_t *src, uint16_t *dst) {
    __m256i s[4];
    s[0] = _mm256_loadu_si256((__m256i *)(src + 0 * 16));
    s[1] = _mm256_loadu_si256((__m256i *)(src + 1 * 16));
    s[2] = _mm256_loadu_si256((__m256i *)(src + 2 * 16));
    s[3] = _mm256_loadu_si256((__m256i *)(src + 3 * 16));
    _mm256_storeu_si256((__m256i *)(dst + 0 * 16), s[0]);
    _mm256_storeu_si256((__m256i *)(dst + 1 * 16), s[1]);
    _mm256_storeu_si256((__m256i *)(dst + 2 * 16), s[2]);
    _mm256_storeu_si256((__m256i *)(dst + 3 * 16), s[3]);
}

static INLINE void copy_128(const uint16_t *src, uint16_t *dst) {
    __m256i s[8];
    s[0] = _mm256_loadu_si256((__m256i *)(src + 0 * 16));
    s[1] = _mm256_loadu_si256((__m256i *)(src + 1 * 16));
    s[2] = _mm256_loadu_si256((__m256i *)(src + 2 * 16));
    s[3] = _mm256_loadu_si256((__m256i *)(src + 3 * 16));
    s[4] = _mm256_loadu_si256((__m256i *)(src + 4 * 16));
    s[5] = _mm256_loadu_si256((__m256i *)(src + 5 * 16));
    s[6] = _mm256_loadu_si256((__m256i *)(src + 6 * 16));
    s[7] = _mm256_loadu_si256((__m256i *)(src + 7 * 16));

    _mm256_storeu_si256((__m256i *)(dst + 0 * 16), s[0]);
    _mm256_storeu_si256((__m256i *)(dst + 1 * 16), s[1]);
    _mm256_storeu_si256((__m256i *)(dst + 2 * 16), s[2]);
    _mm256_storeu_si256((__m256i *)(dst + 3 * 16), s[3]);
    _mm256_storeu_si256((__m256i *)(dst + 4 * 16), s[4]);
    _mm256_storeu_si256((__m256i *)(dst + 5 * 16), s[5]);
    _mm256_storeu_si256((__m256i *)(dst + 6 * 16), s[6]);
    _mm256_storeu_si256((__m256i *)(dst + 7 * 16), s[7]);
}

void svt_av1_highbd_convolve_2d_copy_sr_avx2(const uint16_t *src, int32_t src_stride, uint16_t *dst, int32_t dst_stride,
                                             int32_t w, int32_t h, const InterpFilterParams *filter_params_x,
                                             const InterpFilterParams *filter_params_y, const int32_t subpel_x_q4,
                                             const int32_t subpel_y_q4, ConvolveParams *conv_params, int32_t bd) {
    (void)filter_params_x;
    (void)filter_params_y;
    (void)subpel_x_q4;
    (void)subpel_y_q4;
    (void)conv_params;
    (void)bd;

    if (w == 2) {
        do {
            svt_memcpy_intrin_sse(dst, src, 2 * sizeof(*src));
            src += src_stride;
            dst += dst_stride;
            svt_memcpy_intrin_sse(dst, src, 2 * sizeof(*src));
            src += src_stride;
            dst += dst_stride;
            h -= 2;
        } while (h);
    } else if (w == 4) {
        do {
            __m128i s[2];
            s[0] = _mm_loadl_epi64((__m128i *)src);
            src += src_stride;
            s[1] = _mm_loadl_epi64((__m128i *)src);
            src += src_stride;
            _mm_storel_epi64((__m128i *)dst, s[0]);
            dst += dst_stride;
            _mm_storel_epi64((__m128i *)dst, s[1]);
            dst += dst_stride;
            h -= 2;
        } while (h);
    } else if (w == 8) {
        do {
            __m128i s[2];
            s[0] = _mm_loadu_si128((__m128i *)src);
            src += src_stride;
            s[1] = _mm_loadu_si128((__m128i *)src);
            src += src_stride;
            _mm_storeu_si128((__m128i *)dst, s[0]);
            dst += dst_stride;
            _mm_storeu_si128((__m128i *)dst, s[1]);
            dst += dst_stride;
            h -= 2;
        } while (h);
    } else if (w == 16) {
        do {
            __m256i s[2];
            s[0] = _mm256_loadu_si256((__m256i *)src);
            src += src_stride;
            s[1] = _mm256_loadu_si256((__m256i *)src);
            src += src_stride;
            _mm256_storeu_si256((__m256i *)dst, s[0]);
            dst += dst_stride;
            _mm256_storeu_si256((__m256i *)dst, s[1]);
            dst += dst_stride;
            h -= 2;
        } while (h);
    } else if (w == 32) {
        do {
            __m256i s[4];
            s[0] = _mm256_loadu_si256((__m256i *)(src + 0 * 16));
            s[1] = _mm256_loadu_si256((__m256i *)(src + 1 * 16));
            src += src_stride;
            s[2] = _mm256_loadu_si256((__m256i *)(src + 0 * 16));
            s[3] = _mm256_loadu_si256((__m256i *)(src + 1 * 16));
            src += src_stride;
            _mm256_storeu_si256((__m256i *)(dst + 0 * 16), s[0]);
            _mm256_storeu_si256((__m256i *)(dst + 1 * 16), s[1]);
            dst += dst_stride;
            _mm256_storeu_si256((__m256i *)(dst + 0 * 16), s[2]);
            _mm256_storeu_si256((__m256i *)(dst + 1 * 16), s[3]);
            dst += dst_stride;
            h -= 2;
        } while (h);
    } else if (w == 64) {
        do {
            copy_64(src, dst);
            src += src_stride;
            dst += dst_stride;
            copy_64(src, dst);
            src += src_stride;
            dst += dst_stride;
            h -= 2;
        } while (h);
    } else {
        do {
            copy_128(src, dst);
            src += src_stride;
            dst += dst_stride;
            copy_128(src, dst);
            src += src_stride;
            dst += dst_stride;
            h -= 2;
        } while (h);
    }
}
