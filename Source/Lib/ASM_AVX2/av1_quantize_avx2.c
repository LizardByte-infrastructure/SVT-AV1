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
#include "synonyms_avx2.h"

#include "aom_dsp_rtcd.h"

#define AOM_QM_BITS 5

static INLINE void read_coeff(const TranLow *coeff, __m256i *c) {
    if (sizeof(TranLow) == 4) {
        const __m256i x0 = _mm256_loadu_si256((const __m256i *)coeff);
        const __m256i x1 = _mm256_loadu_si256((const __m256i *)coeff + 1);
        *c               = _mm256_packs_epi32(x0, x1);
        *c               = _mm256_permute4x64_epi64(*c, 0xD8);
    } else
        *c = _mm256_loadu_si256((const __m256i *)coeff);
}

static INLINE void write_zero(TranLow *qcoeff) {
    const __m256i zero = _mm256_setzero_si256();
    if (sizeof(TranLow) == 4) {
        _mm256_storeu_si256((__m256i *)qcoeff, zero);
        _mm256_storeu_si256((__m256i *)qcoeff + 1, zero);
    } else
        _mm256_storeu_si256((__m256i *)qcoeff, zero);
}

static INLINE void init_one_qp(const __m128i *p, __m256i *qp) {
    const __m128i ac = _mm_unpackhi_epi64(*p, *p);
    *qp              = _mm256_insertf128_si256(_mm256_castsi128_si256(*p), ac, 1);
}

static INLINE void init_qp(const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *dequant_ptr,
                           int log_scale, __m256i *thr, __m256i *qp) {
    __m128i       round   = _mm_loadu_si128((const __m128i *)round_ptr);
    const __m128i quant   = _mm_loadu_si128((const __m128i *)quant_ptr);
    const __m128i dequant = _mm_loadu_si128((const __m128i *)dequant_ptr);

    if (log_scale > 0) {
        const __m128i rnd = _mm_set1_epi16((int16_t)1 << (log_scale - 1));
        round             = _mm_add_epi16(round, rnd);
        round             = _mm_srai_epi16(round, log_scale);
    }

    init_one_qp(&round, &qp[0]);
    init_one_qp(&quant, &qp[1]);

    if (log_scale == 1)
        qp[1] = _mm256_slli_epi16(qp[1], log_scale);

    init_one_qp(&dequant, &qp[2]);
    *thr = _mm256_srai_epi16(qp[2], 1 + log_scale);
}

static INLINE void update_qp(int log_scale, __m256i *thr, __m256i *qp) {
    qp[0] = _mm256_permute2x128_si256(qp[0], qp[0], 0x11);
    qp[1] = _mm256_permute2x128_si256(qp[1], qp[1], 0x11);
    qp[2] = _mm256_permute2x128_si256(qp[2], qp[2], 0x11);
    *thr  = _mm256_srai_epi16(qp[2], 1 + log_scale);
}

#define store_quan(q, addr)                                      \
    do {                                                         \
        __m256i sign_bits = _mm256_srai_epi16(q, 15);            \
        __m256i y0        = _mm256_unpacklo_epi16(q, sign_bits); \
        __m256i y1        = _mm256_unpackhi_epi16(q, sign_bits); \
        __m256i x0        = yy_unpacklo_epi128(y0, y1);          \
        __m256i x1        = yy_unpackhi_epi128(y0, y1);          \
        _mm256_storeu_si256((__m256i *)addr, x0);                \
        _mm256_storeu_si256((__m256i *)addr + 1, x1);            \
    } while (0)

#define store_two_quan(q, addr1, dq, addr2)            \
    do {                                               \
        if (sizeof(TranLow) == 4) {                    \
            store_quan(q, addr1);                      \
            store_quan(dq, addr2);                     \
        } else {                                       \
            _mm256_storeu_si256((__m256i *)addr1, q);  \
            _mm256_storeu_si256((__m256i *)addr2, dq); \
        }                                              \
    } while (0)

static INLINE uint16_t quant_gather_eob(__m256i eob) {
    const __m128i eob_lo = _mm256_castsi256_si128(eob);
    const __m128i eob_hi = _mm256_extractf128_si256(eob, 1);
    __m128i       eob_s  = _mm_max_epi16(eob_lo, eob_hi);
    eob_s                = _mm_subs_epu16(_mm_set1_epi16(INT16_MAX), eob_s);
    eob_s                = _mm_minpos_epu16(eob_s);
    return INT16_MAX - _mm_extract_epi16(eob_s, 0);
}

static INLINE void quantize(const __m256i *thr, const __m256i *qp, __m256i *c, const int16_t *iscan_ptr,
                            TranLow *qcoeff, TranLow *dqcoeff, __m256i *eob) {
    const __m256i abs_coeff = _mm256_abs_epi16(*c);
    __m256i       mask      = _mm256_cmpgt_epi16(abs_coeff, *thr);
    mask                    = _mm256_or_si256(mask, _mm256_cmpeq_epi16(abs_coeff, *thr));
    const int nzflag        = _mm256_movemask_epi8(mask);

    if (nzflag) {
        __m256i q        = _mm256_adds_epi16(abs_coeff, qp[0]);
        q                = _mm256_mulhi_epi16(q, qp[1]);
        q                = _mm256_sign_epi16(q, *c);
        const __m256i dq = _mm256_mullo_epi16(q, qp[2]);

        store_two_quan(q, qcoeff, dq, dqcoeff);
        const __m256i zero        = _mm256_setzero_si256();
        const __m256i iscan       = _mm256_loadu_si256((const __m256i *)iscan_ptr);
        const __m256i zero_coeff  = _mm256_cmpeq_epi16(dq, zero);
        const __m256i nzero_coeff = _mm256_cmpeq_epi16(zero_coeff, zero);
        __m256i       cur_eob     = _mm256_sub_epi16(iscan, nzero_coeff);
        cur_eob                   = _mm256_and_si256(cur_eob, nzero_coeff);
        *eob                      = _mm256_max_epi16(*eob, cur_eob);
    } else {
        write_zero(qcoeff);
        write_zero(dqcoeff);
    }
}

void svt_av1_quantize_fp_avx2(const TranLow *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr,
                              const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr,
                              TranLow *qcoeff_ptr, TranLow *dqcoeff_ptr, const int16_t *dequant_ptr, uint16_t *eob_ptr,
                              const int16_t *scan_ptr, const int16_t *iscan_ptr) {
    (void)scan_ptr;
    (void)zbin_ptr;
    (void)quant_shift_ptr;
    const unsigned int step = 16;

    __m256i   qp[3];
    __m256i   coeff, thr;
    const int log_scale = 0;

    init_qp(round_ptr, quant_ptr, dequant_ptr, log_scale, &thr, qp);
    read_coeff(coeff_ptr, &coeff);

    __m256i eob = _mm256_setzero_si256();
    quantize(&thr, qp, &coeff, iscan_ptr, qcoeff_ptr, dqcoeff_ptr, &eob);

    coeff_ptr += step;
    qcoeff_ptr += step;
    dqcoeff_ptr += step;
    iscan_ptr += step;
    n_coeffs -= step;

    update_qp(log_scale, &thr, qp);

    while (n_coeffs > 0) {
        read_coeff(coeff_ptr, &coeff);
        quantize(&thr, qp, &coeff, iscan_ptr, qcoeff_ptr, dqcoeff_ptr, &eob);

        coeff_ptr += step;
        qcoeff_ptr += step;
        dqcoeff_ptr += step;
        iscan_ptr += step;
        n_coeffs -= step;
    }
    *eob_ptr = quant_gather_eob(eob);
}

static INLINE void quantize_32x32(const __m256i *thr, const __m256i *qp, __m256i *c, const int16_t *iscan_ptr,
                                  TranLow *qcoeff, TranLow *dqcoeff, __m256i *eob) {
    const __m256i abs_coeff = _mm256_abs_epi16(*c);
    __m256i       mask      = _mm256_cmpgt_epi16(abs_coeff, *thr);
    mask                    = _mm256_or_si256(mask, _mm256_cmpeq_epi16(abs_coeff, *thr));
    const int nzflag        = _mm256_movemask_epi8(mask);

    if (nzflag) {
        __m256i q = _mm256_adds_epi16(abs_coeff, qp[0]);
        q         = _mm256_mulhi_epu16(q, qp[1]);

        __m256i dq = _mm256_mullo_epi16(q, qp[2]);
        dq         = _mm256_srli_epi16(dq, 1);

        q  = _mm256_sign_epi16(q, *c);
        dq = _mm256_sign_epi16(dq, *c);

        store_two_quan(q, qcoeff, dq, dqcoeff);
        const __m256i zero        = _mm256_setzero_si256();
        const __m256i iscan       = _mm256_loadu_si256((const __m256i *)iscan_ptr);
        const __m256i zero_coeff  = _mm256_cmpeq_epi16(dq, zero);
        const __m256i nzero_coeff = _mm256_cmpeq_epi16(zero_coeff, zero);
        __m256i       cur_eob     = _mm256_sub_epi16(iscan, nzero_coeff);
        cur_eob                   = _mm256_and_si256(cur_eob, nzero_coeff);
        *eob                      = _mm256_max_epi16(*eob, cur_eob);
    } else {
        write_zero(qcoeff);
        write_zero(dqcoeff);
    }
}

void svt_av1_quantize_fp_32x32_avx2(const TranLow *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr,
                                    const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr,
                                    TranLow *qcoeff_ptr, TranLow *dqcoeff_ptr, const int16_t *dequant_ptr,
                                    uint16_t *eob_ptr, const int16_t *scan_ptr, const int16_t *iscan_ptr) {
    (void)scan_ptr;
    (void)zbin_ptr;
    (void)quant_shift_ptr;
    const unsigned int step = 16;

    __m256i   qp[3];
    __m256i   coeff, thr;
    const int log_scale = 1;

    init_qp(round_ptr, quant_ptr, dequant_ptr, log_scale, &thr, qp);
    read_coeff(coeff_ptr, &coeff);

    __m256i eob = _mm256_setzero_si256();
    quantize_32x32(&thr, qp, &coeff, iscan_ptr, qcoeff_ptr, dqcoeff_ptr, &eob);

    coeff_ptr += step;
    qcoeff_ptr += step;
    dqcoeff_ptr += step;
    iscan_ptr += step;
    n_coeffs -= step;

    update_qp(log_scale, &thr, qp);

    while (n_coeffs > 0) {
        read_coeff(coeff_ptr, &coeff);
        quantize_32x32(&thr, qp, &coeff, iscan_ptr, qcoeff_ptr, dqcoeff_ptr, &eob);

        coeff_ptr += step;
        qcoeff_ptr += step;
        dqcoeff_ptr += step;
        iscan_ptr += step;
        n_coeffs -= step;
    }
    *eob_ptr = quant_gather_eob(eob);
}

static INLINE void quantize_64x64(const __m256i *thr, const __m256i *qp, __m256i *c, const int16_t *iscan_ptr,
                                  TranLow *qcoeff, TranLow *dqcoeff, __m256i *eob) {
    const __m256i abs_coeff = _mm256_abs_epi16(*c);
    __m256i       mask      = _mm256_cmpgt_epi16(abs_coeff, *thr);
    mask                    = _mm256_or_si256(mask, _mm256_cmpeq_epi16(abs_coeff, *thr));
    const int nzflag        = _mm256_movemask_epi8(mask);

    if (nzflag) {
        __m256i q         = _mm256_adds_epi16(abs_coeff, qp[0]);
        __m256i qh        = _mm256_mulhi_epi16(q, qp[1]);
        __m256i ql        = _mm256_mullo_epi16(q, qp[1]);
        qh                = _mm256_slli_epi16(qh, 2);
        ql                = _mm256_srli_epi16(ql, 14);
        q                 = _mm256_or_si256(qh, ql);
        const __m256i dqh = _mm256_slli_epi16(_mm256_mulhi_epi16(q, qp[2]), 14);
        const __m256i dql = _mm256_srli_epi16(_mm256_mullo_epi16(q, qp[2]), 2);
        __m256i       dq  = _mm256_or_si256(dqh, dql);

        q  = _mm256_sign_epi16(q, *c);
        dq = _mm256_sign_epi16(dq, *c);

        store_two_quan(q, qcoeff, dq, dqcoeff);
        const __m256i zero        = _mm256_setzero_si256();
        const __m256i iscan       = _mm256_loadu_si256((const __m256i *)iscan_ptr);
        const __m256i zero_coeff  = _mm256_cmpeq_epi16(dq, zero);
        const __m256i nzero_coeff = _mm256_cmpeq_epi16(zero_coeff, zero);
        __m256i       cur_eob     = _mm256_sub_epi16(iscan, nzero_coeff);
        cur_eob                   = _mm256_and_si256(cur_eob, nzero_coeff);
        *eob                      = _mm256_max_epi16(*eob, cur_eob);
    } else {
        write_zero(qcoeff);
        write_zero(dqcoeff);
    }
}

void svt_av1_quantize_fp_64x64_avx2(const TranLow *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr,
                                    const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr,
                                    TranLow *qcoeff_ptr, TranLow *dqcoeff_ptr, const int16_t *dequant_ptr,
                                    uint16_t *eob_ptr, const int16_t *scan_ptr, const int16_t *iscan_ptr) {
    (void)scan_ptr;
    (void)zbin_ptr;
    (void)quant_shift_ptr;
    const unsigned int step = 16;

    __m256i   qp[3];
    __m256i   coeff, thr;
    const int log_scale = 2;

    init_qp(round_ptr, quant_ptr, dequant_ptr, log_scale, &thr, qp);
    read_coeff(coeff_ptr, &coeff);

    __m256i eob = _mm256_setzero_si256();
    quantize_64x64(&thr, qp, &coeff, iscan_ptr, qcoeff_ptr, dqcoeff_ptr, &eob);

    coeff_ptr += step;
    qcoeff_ptr += step;
    dqcoeff_ptr += step;
    iscan_ptr += step;
    n_coeffs -= step;

    update_qp(log_scale, &thr, qp);

    while (n_coeffs > 0) {
        read_coeff(coeff_ptr, &coeff);
        quantize_64x64(&thr, qp, &coeff, iscan_ptr, qcoeff_ptr, dqcoeff_ptr, &eob);

        coeff_ptr += step;
        qcoeff_ptr += step;
        dqcoeff_ptr += step;
        iscan_ptr += step;
        n_coeffs -= step;
    }
    *eob_ptr = quant_gather_eob(eob);
}

static INLINE void init_qp_qm(const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *dequant_ptr,
                              int log_scale, __m256i *thr, __m256i *qp) {
    __m128i       round   = _mm_loadu_si128((const __m128i *)round_ptr);
    const __m128i quant   = _mm_loadu_si128((const __m128i *)quant_ptr);
    const __m128i dequant = _mm_loadu_si128((const __m128i *)dequant_ptr);

    if (log_scale > 0) {
        const __m128i rnd = _mm_set1_epi16((int16_t)1 << (log_scale - 1));
        round             = _mm_add_epi16(round, rnd);
        round             = _mm_srai_epi16(round, log_scale);
    }

    qp[0] = _mm256_cvtepi16_epi32(round);
    qp[1] = _mm256_cvtepi16_epi32(quant);

    qp[2] = _mm256_cvtepi16_epi32(dequant);
    *thr  = _mm256_slli_epi32(qp[2], AOM_QM_BITS - (1 + log_scale));
}

static INLINE void update_qp_qm(int log_scale, __m256i *thr, __m256i *qp) {
    qp[0] = _mm256_permute2x128_si256(qp[0], qp[0], 0x11);
    qp[1] = _mm256_permute2x128_si256(qp[1], qp[1], 0x11);
    qp[2] = _mm256_permute2x128_si256(qp[2], qp[2], 0x11);
    *thr  = _mm256_slli_epi16(qp[2], AOM_QM_BITS - (1 + log_scale));
}
// 64 bit multiply. return the low 64 bits of the intermediate integers
static inline __m256i mm256_mullo_epi64(const __m256i a, const __m256i b) {
    // if a 64bit integer 'a' can be represented by its low 32bit part a0 and high 32bit part a1 as: a1<<32+a0,
    // 64bit integer multiply a*b can expand to: (a1*b1)<<64 + (a1*b0 + a0*b1)<<32 + a0*b0.
    // since only the low 64bit part of the result 128bit integer is needed, the above expression can be simplified as: (a1*b0 + a0*b1)<<32 + a0*b0
    const __m256i bswap   = _mm256_shuffle_epi32(b, 0xB1); // b6 b7 b4 b5 b2 b3 b0 b1
    __m256i       prod_hi = _mm256_mullo_epi32(a,
                                         bswap); // a7*b6 a6*b7 a5*b4 a4*b5 a3*b2 a2*b3 a1*b0 a0*b1
    const __m256i zero    = _mm256_setzero_si256();
    prod_hi               = _mm256_hadd_epi32(prod_hi,
                                zero); // 0 0 a7*b6+a6*b7 a5*b4+a4*b5 0 0 a3*b2+a2*b3 a1*b0+a0*b1
    prod_hi               = _mm256_shuffle_epi32(prod_hi,
                                   0x73); // a7*b6+a6*b7 0 a5*b4+a4*b5 0 a3*b2+a2*b3 0 a1*b0+a0*b1 0
    const __m256i prod_lo = _mm256_mul_epu32(a, b); // 0 a6*b6 0 a4*b4 0 a2*b2 0 a0*b0
    const __m256i prod    = _mm256_add_epi64(prod_lo, prod_hi);
    return prod;
}
static INLINE void clamp_epi32(__m256i *x, __m256i min, __m256i max) {
    *x = _mm256_min_epi32(*x, max);
    *x = _mm256_max_epi32(*x, min);
}
static INLINE void quantize_qm(const __m256i *thr, const __m256i *qp, __m256i *c, const int16_t *iscan_ptr,
                               TranLow *qcoeff, TranLow *dqcoeff, __m256i *eob, const __m256i qm, const __m256i iqm,
                               int16_t log_scale) {
    const __m256i zero      = _mm256_setzero_si256();
    __m256i       min       = _mm256_set1_epi32(INT16_MIN);
    __m256i       max       = _mm256_set1_epi32(INT16_MAX);
    const __m256i abs_coeff = _mm256_abs_epi32(*c);

    __m256i coeff_wt = _mm256_mullo_epi32(abs_coeff, qm);
    __m256i mask     = _mm256_cmpgt_epi32(*thr, coeff_wt);

    const int nzflag = _mm256_movemask_epi8(mask);

    if (EB_LIKELY(~nzflag)) {
        // q*tmp would overflow 32-bit
        const __m256i tmp    = _mm256_mullo_epi32(qm, qp[1]);
        const __m256i tmp_hi = _mm256_srli_epi64(tmp, 32);
        const __m256i tmp_lo = _mm256_srli_epi64(_mm256_slli_epi64(tmp, 32), 32);

        __m256i q = _mm256_add_epi32(abs_coeff, qp[0]);
        clamp_epi32(&q, min, max);
        __m256i q_hi = _mm256_srli_epi64(q, 32);
        __m256i q_lo = _mm256_srli_epi64(_mm256_slli_epi64(q, 32), 32);

        q_lo = mm256_mullo_epi64(q_lo, tmp_lo);
        q_hi = mm256_mullo_epi64(q_hi, tmp_hi);
        q_lo = _mm256_srli_epi64(q_lo, AOM_QM_BITS + 16 - log_scale);
        q_hi = _mm256_srli_epi64(q_hi, AOM_QM_BITS + 16 - log_scale);
        q_hi = _mm256_slli_epi64(q_hi, 32);
        q    = _mm256_or_si256(q_lo, q_hi);

        __m256i       dq  = _mm256_mullo_epi32(qp[2], iqm);
        const __m256i rnd = _mm256_set1_epi32(1 << (AOM_QM_BITS - 1));
        dq                = _mm256_add_epi32(dq, rnd);
        dq                = _mm256_srli_epi32(dq, AOM_QM_BITS);
        dq                = _mm256_mullo_epi32(q, dq);
        dq                = _mm256_srli_epi32(dq, log_scale);

        q  = _mm256_sign_epi32(q, *c);
        dq = _mm256_sign_epi32(dq, *c);

        q  = _mm256_andnot_si256(mask, q);
        dq = _mm256_andnot_si256(mask, dq);

        _mm256_store_si256((__m256i *)qcoeff, q);
        _mm256_store_si256((__m256i *)dqcoeff, dq);

        const __m128i isc   = _mm_loadu_si128((const __m128i *)iscan_ptr);
        const __m256i iscan = _mm256_cvtepi16_epi32(isc);

        const __m256i zc      = _mm256_cmpeq_epi32(dq, zero);
        const __m256i nz      = _mm256_cmpeq_epi32(zc, zero);
        __m256i       cur_eob = _mm256_sub_epi32(iscan, nz);
        cur_eob               = _mm256_and_si256(cur_eob, nz);
        *eob                  = _mm256_max_epi32(cur_eob, *eob);
    } else {
        _mm256_store_si256((__m256i *)qcoeff, zero);
        _mm256_store_si256((__m256i *)dqcoeff, zero);
    }
}

static INLINE __m256i load_bytes_to_m256_avx2(const QmVal *p) {
    __m128i small_load = _mm_loadl_epi64((const __m128i *)p);
    return _mm256_cvtepu8_epi32(small_load);
}

static INLINE int quant_gather_eob_qm(__m256i eob) {
    __m256i eob_s;
    eob_s                   = _mm256_shuffle_epi32(eob, 0xe);
    eob                     = _mm256_max_epi16(eob, eob_s);
    eob_s                   = _mm256_shufflelo_epi16(eob, 0xe);
    eob                     = _mm256_max_epi16(eob, eob_s);
    eob_s                   = _mm256_shufflelo_epi16(eob, 1);
    eob                     = _mm256_max_epi16(eob, eob_s);
    const __m128i final_eob = _mm_max_epi16(_mm256_castsi256_si128(eob), _mm256_extractf128_si256(eob, 1));
    return _mm_extract_epi16(final_eob, 0);
}

void svt_av1_quantize_fp_qm_avx2(const TranLow *coeff_ptr, intptr_t n_coeffs, const int16_t *zbin_ptr,
                                 const int16_t *round_ptr, const int16_t *quant_ptr, const int16_t *quant_shift_ptr,
                                 TranLow *qcoeff_ptr, TranLow *dqcoeff_ptr, const int16_t *dequant_ptr,
                                 uint16_t *eob_ptr, const int16_t *scan_ptr, const int16_t *iscan_ptr,
                                 const QmVal *qm_ptr, const QmVal *iqm_ptr, int16_t log_scale) {
    (void)scan_ptr;
    (void)zbin_ptr;
    (void)quant_shift_ptr;

    const unsigned int step = 8;

    __m256i qp[3], thr;
    init_qp_qm(round_ptr, quant_ptr, dequant_ptr, log_scale, &thr, qp);
    __m256i eob = _mm256_setzero_si256();

    __m256i coeff = _mm256_load_si256((const __m256i *)coeff_ptr);
    __m256i qm    = load_bytes_to_m256_avx2(qm_ptr);
    __m256i iqm   = load_bytes_to_m256_avx2(iqm_ptr);
    quantize_qm(&thr, qp, &coeff, iscan_ptr, qcoeff_ptr, dqcoeff_ptr, &eob, qm, iqm, log_scale);

    update_qp_qm(log_scale, &thr, qp);
    while (n_coeffs > step) {
        coeff_ptr += step;
        qcoeff_ptr += step;
        dqcoeff_ptr += step;
        iscan_ptr += step;
        qm_ptr += step;
        iqm_ptr += step;
        n_coeffs -= step;

        qm    = load_bytes_to_m256_avx2(qm_ptr);
        iqm   = load_bytes_to_m256_avx2(iqm_ptr);
        coeff = _mm256_load_si256((const __m256i *)coeff_ptr);
        quantize_qm(&thr, qp, &coeff, iscan_ptr, qcoeff_ptr, dqcoeff_ptr, &eob, qm, iqm, log_scale);
    }
    *eob_ptr = quant_gather_eob_qm(eob);
}
