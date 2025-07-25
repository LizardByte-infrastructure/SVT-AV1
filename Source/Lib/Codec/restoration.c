/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
 *
 */
#include "av1_common.h"
#include "common_dsp_rtcd.h"
#include "restoration.h"
#include "utility.h"
#include "svt_log.h"
#include "intra_prediction.h"
#include "pcs.h"
#include "super_res.h"
#include "pic_operators.h"
#include "convolve.h"

int32_t svt_aom_realloc_frame_buffer(Yv12BufferConfig *ybf, int32_t width, int32_t height, int32_t ss_x, int32_t ss_y,
                                     int32_t use_highbitdepth, int32_t border, int32_t byte_alignment,
                                     AomCodecFrameBuffer *fb, AomGetFrameBufferCbFn cb, void *cb_priv);

// The 's' values are calculated based on original 'r' and 'e' values in the
// spec using GenSgrprojVtable().
// Note: Setting r = 0 skips the filter; with corresponding s = -1 (invalid).
//n = (2 * r + 1) * (2 * r + 1);
//n2e = n * n * ep;
//s = (((1 << SGRPROJ_MTABLE_BITS) + n2e / 2) / n2e);

const SgrParamsType svt_aom_eb_sgr_params[SGRPROJ_PARAMS] = {
    //      r0 e0  r1 e1
    {{2, 1}, {140, 3236}}, // 0  { 2, 12, 1, 4  }
    {{2, 1}, {112, 2158}}, // 1  { 2, 15, 1, 6  }
    {{2, 1}, {93, 1618}}, // 2  { 2, 18, 1, 8  }
    {{2, 1}, {80, 1438}}, // 3  { 2, 21, 1, 9  }
    {{2, 1}, {70, 1295}}, // 4  { 2, 24, 1, 10 }
    {{2, 1}, {58, 1177}}, // 5  { 2, 29, 1, 11 }
    {{2, 1}, {47, 1079}}, // 6  { 2, 36, 1, 12 }
    {{2, 1}, {37, 996}}, // 7  { 2, 45, 1, 13 }
    {{2, 1}, {30, 925}}, // 8  { 2, 56, 1, 14 }
    {{2, 1}, {25, 863}}, // 9  { 2, 68, 1, 15 }
    {{0, 1}, {-1, 2589}}, // 10 { 0, 0,  1, 5  }
    {{0, 1}, {-1, 1618}}, // 11 { 0, 0,  1, 8  }
    {{0, 1}, {-1, 1177}}, // 12 { 0, 0,  1, 11 }
    {{0, 1}, {-1, 925}}, // 13 { 0, 0,  1, 14 }
    {{2, 0}, {56, -1}}, // 14 { 2, 30, 0, 0  }
    {{2, 0}, {22, -1}}, // 15 { 2, 75, 0, 0  }
};
Av1PixelRect svt_aom_whole_frame_rect(FrameSize *frm_size, int32_t sub_x, int32_t sub_y, int32_t is_uv) {
    Av1PixelRect rect;

    int32_t ss_x = is_uv && sub_x;
    int32_t ss_y = is_uv && sub_y;

    rect.top    = 0;
    rect.bottom = ROUND_POWER_OF_TWO(frm_size->frame_height, ss_y);
    rect.left   = 0;
    rect.right  = ROUND_POWER_OF_TWO(frm_size->superres_upscaled_width, ss_x);
    return rect;
}

// Count horizontal or vertical units per tile (use a width or height for
// tile_size, respectively). We basically want to divide the tile size by the
// size of a restoration unit. Rather than rounding up unconditionally as you
// might expect, we round to nearest, which models the way a right or bottom
// restoration unit can extend to up to 150% its normal width or height. The
// max with 1 is to deal with tiles that are smaller than half of a restoration
// unit.
static int32_t count_units_in_tile(int32_t unit_size, int32_t tile_size) {
    return AOMMAX((tile_size + (unit_size >> 1)) / unit_size, 1);
}

EbErrorType svt_av1_alloc_restoration_struct(struct Av1Common *cm, RestorationInfo *rsi, int32_t is_uv) {
    // We need to allocate enough space for restoration units to cover the
    // largest tile. Without CONFIG_MAX_TILE, this is always the tile at the
    // top-left and we can use av1_get_tile_rect(). With CONFIG_MAX_TILE, we have
    // to do the computation ourselves, iterating over the tiles and keeping
    // track of the largest width and height, then upscaling.
    const Av1PixelRect tile_rect = svt_aom_whole_frame_rect(&cm->frm_size, cm->subsampling_x, cm->subsampling_y, is_uv);
    const int32_t      max_tile_w = tile_rect.right - tile_rect.left;
    const int32_t      max_tile_h = tile_rect.bottom - tile_rect.top;

    // To calculate hpertile and vpertile (horizontal and vertical units per
    // tile), we basically want to divide the largest tile width or height by the
    // size of a restoration unit. Rather than rounding up unconditionally as you
    // might expect, we round to nearest, which models the way a right or bottom
    // restoration unit can extend to up to 150% its normal width or height. The
    // max with 1 is to deal with tiles that are smaller than half of a
    // restoration unit.
    const int32_t unit_size = rsi->restoration_unit_size;
    const int32_t hpertile  = count_units_in_tile(
        unit_size,
        max_tile_w); //FB of size < 1/2 unit_size are included in neigh FB making them bigger!!
    const int32_t vpertile = count_units_in_tile(unit_size, max_tile_h);

    rsi->units_per_tile      = hpertile * vpertile; //pic_tot_FB
    rsi->horz_units_per_tile = hpertile; //pic_width_in_FB
    rsi->vert_units_per_tile = vpertile; //pic_height_in_FB

    const int32_t ntiles = 1;
    const int32_t nunits = ntiles * rsi->units_per_tile;

    EB_MALLOC_ARRAY(rsi->unit_info, nunits);

    return EB_ErrorNone;
}

static void extend_frame_lowbd(uint8_t *data, int32_t width, int32_t height, int32_t stride, int32_t border_horz,
                               int32_t border_vert) {
    uint8_t *data_p;
    int32_t  i;
    for (i = 0; i < height; ++i) {
        data_p = data + i * stride;
        memset(data_p - border_horz, data_p[0], border_horz);
        memset(data_p + width, data_p[width - 1], border_horz);
    }
    data_p = data - border_horz;
    for (i = -border_vert; i < 0; ++i) svt_memcpy(data_p + i * stride, data_p, width + 2 * border_horz);
    for (i = height; i < height + border_vert; ++i) {
        svt_memcpy(data_p + i * stride, data_p + (height - 1) * stride, width + 2 * border_horz);
    }
}

static void extend_frame_highbd(uint16_t *data, int32_t width, int32_t height, int32_t stride, int32_t border_horz,
                                int32_t border_vert) {
    uint16_t *data_p;
    int32_t   i, j;
    for (i = 0; i < height; ++i) {
        data_p = data + i * stride;
        for (j = -border_horz; j < 0; ++j) data_p[j] = data_p[0];
        for (j = width; j < width + border_horz; ++j) data_p[j] = data_p[width - 1];
    }
    data_p = data - border_horz;
    for (i = -border_vert; i < 0; ++i) {
        svt_memcpy(data_p + i * stride, data_p, (width + 2 * border_horz) * sizeof(uint16_t));
    }
    for (i = height; i < height + border_vert; ++i) {
        svt_memcpy(data_p + i * stride, data_p + (height - 1) * stride, (width + 2 * border_horz) * sizeof(uint16_t));
    }
}

void svt_extend_frame(uint8_t *data, int32_t width, int32_t height, int32_t stride, int32_t border_horz,
                      int32_t border_vert, int32_t highbd) {
    if (highbd)
        extend_frame_highbd(CONVERT_TO_SHORTPTR(data), width, height, stride, border_horz, border_vert);
    else
        extend_frame_lowbd(data, width, height, stride, border_horz, border_vert);
}

static void copy_tile_lowbd(int32_t width, int32_t height, const uint8_t *src, int32_t src_stride, uint8_t *dst,
                            int32_t dst_stride) {
    for (int32_t i = 0; i < height; ++i) svt_memcpy(dst + i * dst_stride, src + i * src_stride, width);
}

static void copy_tile_highbd(int32_t width, int32_t height, const uint16_t *src, int32_t src_stride, uint16_t *dst,
                             int32_t dst_stride) {
    for (int32_t i = 0; i < height; ++i) svt_memcpy(dst + i * dst_stride, src + i * src_stride, width * sizeof(*dst));
}

void svt_aom_copy_tile(int32_t width, int32_t height, const uint8_t *src, int32_t src_stride, uint8_t *dst,
                       int32_t dst_stride, int32_t highbd) {
    if (highbd)
        copy_tile_highbd(width, height, CONVERT_TO_SHORTPTR(src), src_stride, CONVERT_TO_SHORTPTR(dst), dst_stride);
    else
        copy_tile_lowbd(width, height, src, src_stride, dst, dst_stride);
}

// With striped loop restoration, the filtering for each 64-pixel stripe gets
// most of its input from the output of CDEF (stored in data8), but we need to
// fill out a border of 3 pixels above/below the stripe according to the
// following
// rules:
//
// * At a frame boundary, we copy the outermost row of CDEF pixels three times.
//   This extension is done by a call to svt_extend_frame() at the start of the loop
//   restoration process, so the value of copy_above/copy_below doesn't strictly
//   matter.
//   However, by setting *copy_above = *copy_below = 1 whenever loop filtering
//   across tiles is disabled, we can allow
//   {setup,restore}_processing_stripe_boundary to assume that the top/bottom
//   data has always been copied, simplifying the behaviour at the left and
//   right edges of tiles.
//
// * If we're at a tile boundary and loop filtering across tiles is enabled,
//   then there is a logical stripe which is 64 pixels high, but which is split
//   into an 8px high and a 56px high stripe so that the processing (and
//   coefficient set usage) can be aligned to tiles.
//   In this case, we use the 3 rows of CDEF output across the boundary for
//   context; this corresponds to leaving the frame buffer as-is.
//
// * If we're at a tile boundary and loop filtering across tiles is disabled,
//   then we take the outermost row of CDEF pixels *within the current tile*
//   and copy it three times. Thus we behave exactly as if the tile were a full
//   frame.
//
// * Otherwise, we're at a stripe boundary within a tile. In that case, we
//   take 2 rows of deblocked pixels and extend them to 3 rows of context.
//
// The distinction between the latter two cases is handled by the
// svt_av1_loop_restoration_save_boundary_lines() function, so here we just need
// to decide if we're overwriting the above/below boundary pixels or not.
void svt_aom_get_stripe_boundary_info(const RestorationTileLimits *limits, const Av1PixelRect *tile_rect, int32_t ss_y,
                                      int32_t *copy_above, int32_t *copy_below) {
    *copy_above = 1;
    *copy_below = 1;

    const int32_t full_stripe_height = RESTORATION_PROC_UNIT_SIZE >> ss_y;
    const int32_t runit_offset       = RESTORATION_UNIT_OFFSET >> ss_y;

    const int32_t first_stripe_in_tile = (limits->v_start == tile_rect->top);
    const int32_t this_stripe_height   = full_stripe_height - (first_stripe_in_tile ? runit_offset : 0);
    const int32_t last_stripe_in_tile  = (limits->v_start + this_stripe_height >= tile_rect->bottom);

    if (first_stripe_in_tile)
        *copy_above = 0;
    if (last_stripe_in_tile)
        *copy_below = 0;
}

// Overwrite the border pixels around a processing stripe so that the conditions
// listed above svt_aom_get_stripe_boundary_info() are preserved.
// We save the pixels which get overwritten into a temporary buffer, so that
// they can be restored by svt_aom_restore_processing_stripe_boundary() after we've
// processed the stripe.
//
// limits gives the rectangular limits of the remaining stripes for the current
// restoration unit. rsb is the stored stripe boundaries (taken from either
// deblock or CDEF output as necessary).
//
// tile_rect is the limits of the current tile and tile_stripe0 is the index of
// the first stripe in this tile (needed to convert the tile-relative stripe
// index we get from limits into something we can look up in rsb).
void svt_aom_setup_processing_stripe_boundary(const RestorationTileLimits       *limits,
                                              const RestorationStripeBoundaries *rsb, int32_t rsb_row,
                                              int32_t use_highbd, int32_t h, uint8_t *data8, int32_t data_stride,
                                              RestorationLineBuffers *rlbs, int32_t copy_above, int32_t copy_below,
                                              int32_t opt) {
    // Offsets within the line buffers. The buffer logically starts at column
    // -RESTORATION_EXTRA_HORZ so the 1st column (at x0 - RESTORATION_EXTRA_HORZ)
    // has column x0 in the buffer.
    const int32_t buf_stride = rsb->stripe_boundary_stride;
    const int32_t buf_x0_off = limits->h_start;
    const int32_t line_width = (limits->h_end - limits->h_start) + 2 * RESTORATION_EXTRA_HORZ;
    const int32_t line_size  = line_width << use_highbd;

    const int32_t data_x0 = limits->h_start - RESTORATION_EXTRA_HORZ;

    // Replace RESTORATION_BORDER pixels above the top of the stripe
    // We expand RESTORATION_CTX_VERT=2 lines from rsb->stripe_boundary_above
    // to fill RESTORATION_BORDER=3 lines of above pixels. This is done by
    // duplicating the topmost of the 2 lines (see the AOMMAX call when
    // calculating src_row, which gets the values 0, 0, 1 for i = -3, -2, -1).
    //
    // Special case: If we're at the top of a tile, which isn't on the topmost
    // tile row, and we're allowed to loop filter across tiles, then we have a
    // logical 64-pixel-high stripe which has been split into an 8-pixel high
    // stripe and a 56-pixel high stripe (the current one). So, in this case,
    // we want to leave the boundary alone!
    if (!opt) {
        if (copy_above) {
            uint8_t *data8_tl = data8 + data_x0 + limits->v_start * data_stride;

            for (int32_t i = -RESTORATION_BORDER; i < 0; ++i) {
                const int32_t  buf_row = rsb_row + AOMMAX(i + RESTORATION_CTX_VERT, 0);
                const int32_t  buf_off = buf_x0_off + buf_row * buf_stride;
                const uint8_t *buf     = rsb->stripe_boundary_above + (buf_off << use_highbd);
                uint8_t       *dst8    = data8_tl + i * data_stride;
                // Save old pixels, then replace with data from stripe_boundary_above
                svt_memcpy(rlbs->tmp_save_above[i + RESTORATION_BORDER], REAL_PTR(use_highbd, dst8), line_size);
                svt_memcpy(REAL_PTR(use_highbd, dst8), buf, line_size);
            }
        }

        // Replace RESTORATION_BORDER pixels below the bottom of the stripe.
        // The second buffer row is repeated, so src_row gets the values 0, 1, 1
        // for i = 0, 1, 2.
        if (copy_below) {
            const int32_t stripe_end = limits->v_start + h;
            uint8_t      *data8_bl   = data8 + data_x0 + stripe_end * data_stride;

            for (int32_t i = 0; i < RESTORATION_BORDER; ++i) {
                const int32_t  buf_row = rsb_row + AOMMIN(i, RESTORATION_CTX_VERT - 1);
                const int32_t  buf_off = buf_x0_off + buf_row * buf_stride;
                const uint8_t *src     = rsb->stripe_boundary_below + (buf_off << use_highbd);

                uint8_t *dst8 = data8_bl + i * data_stride;
                // Save old pixels, then replace with data from stripe_boundary_below
                svt_memcpy(rlbs->tmp_save_below[i], REAL_PTR(use_highbd, dst8), line_size);
                svt_memcpy(REAL_PTR(use_highbd, dst8), src, line_size);
            }
        }
    } else {
        if (copy_above) {
            uint8_t *data8_tl = data8 + data_x0 + limits->v_start * data_stride;

            // Only save and overwrite i=-RESTORATION_BORDER line.
            uint8_t *dst8 = data8_tl + (-RESTORATION_BORDER) * data_stride;
            // Save old pixels, then replace with data from stripe_boundary_above
            svt_memcpy(rlbs->tmp_save_above[0], REAL_PTR(use_highbd, dst8), line_size);
            svt_memcpy(REAL_PTR(use_highbd, dst8),
                       REAL_PTR(use_highbd, data8_tl + (-RESTORATION_BORDER + 1) * data_stride),
                       line_size);
        }

        if (copy_below) {
            const int32_t stripe_end = limits->v_start + h;
            uint8_t      *data8_bl   = data8 + data_x0 + stripe_end * data_stride;

            // Only save and overwrite i=2 line.
            uint8_t *dst8 = data8_bl + 2 * data_stride;
            // Save old pixels, then replace with data from stripe_boundary_below
            svt_memcpy(rlbs->tmp_save_below[2], REAL_PTR(use_highbd, dst8), line_size);
            svt_memcpy(REAL_PTR(use_highbd, dst8), REAL_PTR(use_highbd, data8_bl + (2 - 1) * data_stride), line_size);
        }
    }
}

// This function restores the boundary lines modified by
// svt_aom_setup_processing_stripe_boundary.
//
// Note: We need to be careful when handling the corners of the processing
// unit, because (eg.) the top-left corner is considered to be part of
// both the left and top borders. This means that, depending on the
// loop_filter_across_tiles_enabled flag, the corner pixels might get
// overwritten twice, once as part of the "top" border and once as part
// of the "left" border (or similar for other corners).
//
// Everything works out fine as long as we make sure to reverse the order
// when restoring, ie. we need to restore the left/right borders followed
// by the top/bottom borders.
void svt_aom_restore_processing_stripe_boundary(const RestorationTileLimits *limits, const RestorationLineBuffers *rlbs,
                                                int32_t use_highbd, int32_t h, uint8_t *data8, int32_t data_stride,
                                                int32_t copy_above, int32_t copy_below, int32_t opt) {
    const int32_t line_width = (limits->h_end - limits->h_start) + 2 * RESTORATION_EXTRA_HORZ;
    const int32_t line_size  = line_width << use_highbd;

    const int32_t data_x0 = limits->h_start - RESTORATION_EXTRA_HORZ;

    if (!opt) {
        if (copy_above) {
            uint8_t *data8_tl = data8 + data_x0 + limits->v_start * data_stride;
            for (int32_t i = -RESTORATION_BORDER; i < 0; ++i) {
                uint8_t *dst8 = data8_tl + i * data_stride;
                svt_memcpy(REAL_PTR(use_highbd, dst8), rlbs->tmp_save_above[i + RESTORATION_BORDER], line_size);
            }
        }

        if (copy_below) {
            const int32_t stripe_bottom = limits->v_start + h;
            uint8_t      *data8_bl      = data8 + data_x0 + stripe_bottom * data_stride;

            for (int32_t i = 0; i < RESTORATION_BORDER; ++i) {
                if (stripe_bottom + i >= limits->v_end + RESTORATION_BORDER)
                    break;

                uint8_t *dst8 = data8_bl + i * data_stride;
                svt_memcpy(REAL_PTR(use_highbd, dst8), rlbs->tmp_save_below[i], line_size);
            }
        }
    } else {
        if (copy_above) {
            uint8_t *data8_tl = data8 + data_x0 + limits->v_start * data_stride;

            // Only restore i=-RESTORATION_BORDER line.
            uint8_t *dst8 = data8_tl + (-RESTORATION_BORDER) * data_stride;
            svt_memcpy(REAL_PTR(use_highbd, dst8), rlbs->tmp_save_above[0], line_size);
        }

        if (copy_below) {
            const int32_t stripe_bottom = limits->v_start + h;
            uint8_t      *data8_bl      = data8 + data_x0 + stripe_bottom * data_stride;

            // Only restore i=2 line.
            if (stripe_bottom + 2 < limits->v_end + RESTORATION_BORDER) {
                uint8_t *dst8 = data8_bl + 2 * data_stride;
                svt_memcpy(REAL_PTR(use_highbd, dst8), rlbs->tmp_save_below[2], line_size);
            }
        }
    }
}

void svt_aom_wiener_filter_stripe(const RestorationUnitInfo *rui, int32_t stripe_width, int32_t stripe_height,
                                  int32_t procunit_width, const uint8_t *src, int32_t src_stride, uint8_t *dst,
                                  int32_t dst_stride, int32_t *tmpbuf, int32_t bit_depth) {
    (void)tmpbuf;
    (void)bit_depth;
    assert(bit_depth == 8);
    const ConvolveParams conv_params = get_conv_params_wiener(8);

    for (int32_t j = 0; j < stripe_width; j += procunit_width) {
        int32_t        w     = AOMMIN(procunit_width, (stripe_width - j + 15) & ~15);
        const uint8_t *src_p = src + j;
        uint8_t       *dst_p = dst + j; //CHKN  SSE
        svt_av1_wiener_convolve_add_src(src_p,
                                        src_stride,
                                        dst_p,
                                        dst_stride,
                                        rui->wiener_info.hfilter,
                                        rui->wiener_info.vfilter,
                                        w,
                                        stripe_height,
                                        &conv_params);
    }
}

/* Calculate windowed sums (if sqr=0) or sums of squares (if sqr=1)
   over the input. The window is of size (2r + 1)x(2r + 1), and we
   specialize to r = 1, 2, 3. A default function is used for r > 3.

   Each loop follows the same format: We keep a window's worth of input
   in individual variables and select data out of that as appropriate.
*/
static void boxsum1(int32_t *src, int32_t width, int32_t height, int32_t src_stride, int32_t sqr, int32_t *dst,
                    int32_t dst_stride) {
    int32_t i, j, a, b, c;
    assert(width > 2 * SGRPROJ_BORDER_HORZ);
    assert(height > 2 * SGRPROJ_BORDER_VERT);

    // Vertical sum over 3-pixel regions, from src into dst.
    if (!sqr) {
        for (j = 0; j < width; ++j) {
            a = src[j];
            b = src[src_stride + j];
            c = src[2 * src_stride + j];

            dst[j] = a + b;
            for (i = 1; i < height - 2; ++i) {
                // Loop invariant: At the start of each iteration,
                // a = src[(i - 1) * src_stride + j]
                // b = src[(i    ) * src_stride + j]
                // c = src[(i + 1) * src_stride + j]
                dst[i * dst_stride + j] = a + b + c;
                a                       = b;
                b                       = c;
                c                       = src[(i + 2) * src_stride + j];
            }
            dst[i * dst_stride + j]       = a + b + c;
            dst[(i + 1) * dst_stride + j] = b + c;
        }
    } else {
        for (j = 0; j < width; ++j) {
            a = src[j] * src[j];
            b = src[src_stride + j] * src[src_stride + j];
            c = src[2 * src_stride + j] * src[2 * src_stride + j];

            dst[j] = a + b;
            for (i = 1; i < height - 2; ++i) {
                dst[i * dst_stride + j] = a + b + c;
                a                       = b;
                b                       = c;
                c                       = src[(i + 2) * src_stride + j] * src[(i + 2) * src_stride + j];
            }
            dst[i * dst_stride + j]       = a + b + c;
            dst[(i + 1) * dst_stride + j] = b + c;
        }
    }

    // Horizontal sum over 3-pixel regions of dst
    for (i = 0; i < height; ++i) {
        a = dst[i * dst_stride];
        b = dst[i * dst_stride + 1];
        c = dst[i * dst_stride + 2];

        dst[i * dst_stride] = a + b;
        for (j = 1; j < width - 2; ++j) {
            // Loop invariant: At the start of each iteration,
            // a = src[i * src_stride + (j - 1)]
            // b = src[i * src_stride + (j    )]
            // c = src[i * src_stride + (j + 1)]
            dst[i * dst_stride + j] = a + b + c;
            a                       = b;
            b                       = c;
            c                       = dst[i * dst_stride + (j + 2)];
        }
        dst[i * dst_stride + j]       = a + b + c;
        dst[i * dst_stride + (j + 1)] = b + c;
    }
}

static void boxsum2(int32_t *src, int32_t width, int32_t height, int32_t src_stride, int32_t sqr, int32_t *dst,
                    int32_t dst_stride) {
    int32_t i, j, a, b, c, d, e;
    assert(width > 2 * SGRPROJ_BORDER_HORZ);
    assert(height > 2 * SGRPROJ_BORDER_VERT);

    // Vertical sum over 5-pixel regions, from src into dst.
    if (!sqr) {
        for (j = 0; j < width; ++j) {
            a = src[j];
            b = src[src_stride + j];
            c = src[2 * src_stride + j];
            d = src[3 * src_stride + j];
            e = src[4 * src_stride + j];

            dst[j]              = a + b + c;
            dst[dst_stride + j] = a + b + c + d;
            for (i = 2; i < height - 3; ++i) {
                // Loop invariant: At the start of each iteration,
                // a = src[(i - 2) * src_stride + j]
                // b = src[(i - 1) * src_stride + j]
                // c = src[(i    ) * src_stride + j]
                // d = src[(i + 1) * src_stride + j]
                // e = src[(i + 2) * src_stride + j]
                dst[i * dst_stride + j] = a + b + c + d + e;
                a                       = b;
                b                       = c;
                c                       = d;
                d                       = e;
                e                       = src[(i + 3) * src_stride + j];
            }
            dst[i * dst_stride + j]       = a + b + c + d + e;
            dst[(i + 1) * dst_stride + j] = b + c + d + e;
            dst[(i + 2) * dst_stride + j] = c + d + e;
        }
    } else {
        for (j = 0; j < width; ++j) {
            a = src[j] * src[j];
            b = src[src_stride + j] * src[src_stride + j];
            c = src[2 * src_stride + j] * src[2 * src_stride + j];
            d = src[3 * src_stride + j] * src[3 * src_stride + j];
            e = src[4 * src_stride + j] * src[4 * src_stride + j];

            dst[j]              = a + b + c;
            dst[dst_stride + j] = a + b + c + d;
            for (i = 2; i < height - 3; ++i) {
                dst[i * dst_stride + j] = a + b + c + d + e;
                a                       = b;
                b                       = c;
                c                       = d;
                d                       = e;
                e                       = src[(i + 3) * src_stride + j] * src[(i + 3) * src_stride + j];
            }
            dst[i * dst_stride + j]       = a + b + c + d + e;
            dst[(i + 1) * dst_stride + j] = b + c + d + e;
            dst[(i + 2) * dst_stride + j] = c + d + e;
        }
    }

    // Horizontal sum over 5-pixel regions of dst
    for (i = 0; i < height; ++i) {
        a = dst[i * dst_stride];
        b = dst[i * dst_stride + 1];
        c = dst[i * dst_stride + 2];
        d = dst[i * dst_stride + 3];
        e = dst[i * dst_stride + 4];

        dst[i * dst_stride]     = a + b + c;
        dst[i * dst_stride + 1] = a + b + c + d;
        for (j = 2; j < width - 3; ++j) {
            // Loop invariant: At the start of each iteration,
            // a = src[i * src_stride + (j - 2)]
            // b = src[i * src_stride + (j - 1)]
            // c = src[i * src_stride + (j    )]
            // d = src[i * src_stride + (j + 1)]
            // e = src[i * src_stride + (j + 2)]
            dst[i * dst_stride + j] = a + b + c + d + e;
            a                       = b;
            b                       = c;
            c                       = d;
            d                       = e;
            e                       = dst[i * dst_stride + (j + 3)];
        }
        dst[i * dst_stride + j]       = a + b + c + d + e;
        dst[i * dst_stride + (j + 1)] = b + c + d + e;
        dst[i * dst_stride + (j + 2)] = c + d + e;
    }
}

static void boxsum(int32_t *src, int32_t width, int32_t height, int32_t src_stride, int32_t r, int32_t sqr,
                   int32_t *dst, int32_t dst_stride) {
    if (r == 1)
        boxsum1(src, width, height, src_stride, sqr, dst, dst_stride);
    else if (r == 2)
        boxsum2(src, width, height, src_stride, sqr, dst, dst_stride);
    else
        assert(0 && "Invalid value of r in self-guided filter");
}

void svt_decode_xq(const int32_t *xqd, int32_t *xq, const SgrParamsType *params) {
    if (params->r[0] == 0) {
        xq[0] = 0;
        xq[1] = (1 << SGRPROJ_PRJ_BITS) - xqd[1];
    } else if (params->r[1] == 0) {
        xq[0] = xqd[0];
        xq[1] = 0;
    } else {
        xq[0] = xqd[0];
        xq[1] = (1 << SGRPROJ_PRJ_BITS) - xq[0] - xqd[1];
    }
}

const int32_t svt_aom_eb_x_by_xplus1[256] = {
    // Special case: Map 0 -> 1 (corresponding to a value of 1/256)
    // instead of 0. See comments in selfguided_restoration_internal() for why
    1,   128, 171, 192, 205, 213, 219, 224, 228, 230, 233, 235, 236, 238, 239, 240, 241, 242, 243, 243, 244, 244,
    245, 245, 246, 246, 247, 247, 247, 247, 248, 248, 248, 248, 249, 249, 249, 249, 249, 250, 250, 250, 250, 250,
    250, 250, 251, 251, 251, 251, 251, 251, 251, 251, 251, 251, 252, 252, 252, 252, 252, 252, 252, 252, 252, 252,
    252, 252, 252, 252, 252, 252, 252, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253,
    253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 253, 254, 254, 254, 254, 254, 254, 254, 254,
    254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254,
    254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254,
    254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 254, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255,
    255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 256,
};

const int32_t svt_aom_eb_one_by_x[MAX_NELEM] = {
    4096, 2048, 1365, 1024, 819, 683, 585, 512, 455, 410, 372, 341, 315,
    293,  273,  256,  241,  228, 216, 205, 195, 186, 178, 171, 164,
};

static void selfguided_restoration_fast_internal(int32_t *dgd, int32_t width, int32_t height, int32_t dgd_stride,
                                                 int32_t *dst, int32_t dst_stride, int32_t bit_depth,
                                                 int32_t sgr_params_idx, int32_t radius_idx) {
    const SgrParamsType *const params     = &svt_aom_eb_sgr_params[sgr_params_idx];
    const int32_t              r          = params->r[radius_idx];
    const int32_t              width_ext  = width + 2 * SGRPROJ_BORDER_HORZ;
    const int32_t              height_ext = height + 2 * SGRPROJ_BORDER_VERT;
    // Adjusting the stride of A and B here appears to avoid bad cache effects,
    // leading to a significant speed improvement.
    // We also align the stride to a multiple of 16 bytes, for consistency
    // with the SIMD version of this function.
    int32_t  buf_stride = ((width_ext + 3) & ~3) + 16;
    int32_t  a_[RESTORATION_PROC_UNIT_PELS];
    int32_t  b_[RESTORATION_PROC_UNIT_PELS];
    int32_t *A = a_;
    int32_t *B = b_;
    int32_t  i, j;

    assert(r <= MAX_RADIUS && "Need MAX_RADIUS >= r");
    assert(r <= SGRPROJ_BORDER_VERT - 1 && r <= SGRPROJ_BORDER_HORZ - 1 && "Need SGRPROJ_BORDER_* >= r+1");

    boxsum(dgd - dgd_stride * SGRPROJ_BORDER_VERT - SGRPROJ_BORDER_HORZ,
           width_ext,
           height_ext,
           dgd_stride,
           r,
           0,
           B,
           buf_stride);
    boxsum(dgd - dgd_stride * SGRPROJ_BORDER_VERT - SGRPROJ_BORDER_HORZ,
           width_ext,
           height_ext,
           dgd_stride,
           r,
           1,
           A,
           buf_stride);
    A += SGRPROJ_BORDER_VERT * buf_stride + SGRPROJ_BORDER_HORZ;
    B += SGRPROJ_BORDER_VERT * buf_stride + SGRPROJ_BORDER_HORZ;
    // Calculate the eventual A[] and B[] arrays. Include a 1-pixel border - ie,
    // for a 64x64 processing unit, we calculate 66x66 pixels of A[] and B[].
    for (i = -1; i < height + 1; i += 2) {
        for (j = -1; j < width + 1; ++j) {
            const int32_t k = i * buf_stride + j;
            const int32_t n = (2 * r + 1) * (2 * r + 1);

            // a < 2^16 * n < 2^22 regardless of bit depth
            uint32_t a = ROUND_POWER_OF_TWO(A[k], 2 * (bit_depth - 8));
            // b < 2^8 * n < 2^14 regardless of bit depth
            uint32_t b = ROUND_POWER_OF_TWO(B[k], bit_depth - 8);

            // Each term in calculating p = a * n - b * b is < 2^16 * n^2 < 2^28,
            // and p itself satisfies p < 2^14 * n^2 < 2^26.
            // This bound on p is due to:
            // https://en.wikipedia.org/wiki/Popoviciu's_inequality_on_variances
            //
            // Note: Sometimes, in high bit depth, we can end up with a*n < b*b.
            // This is an artefact of rounding, and can only happen if all pixels
            // are (almost) identical, so in this case we saturate to p=0.
            uint32_t p = (a * n < b * b) ? 0 : a * n - b * b;

            const uint32_t s = params->s[radius_idx];

            // p * s < (2^14 * n^2) * round(2^20 / n^2 eps) < 2^34 / eps < 2^32
            // as long as eps >= 4. So p * s fits into a uint32_t, and z < 2^12
            // (this holds even after accounting for the rounding in s)
            const uint32_t z = ROUND_POWER_OF_TWO(p * s, SGRPROJ_MTABLE_BITS);

            // Note: We have to be quite careful about the value of A[k].
            // This is used as a blend factor between individual pixel values and the
            // local mean. So it logically has a range of [0, 256], including both
            // endpoints.
            //
            // This is a pain for hardware, as we'd like something which can be stored
            // in exactly 8 bits.
            // Further, in the calculation of B[k] below, if z == 0 and r == 2,
            // then A[k] "should be" 0. But then we can end up setting B[k] to a value
            // slightly above 2^(8 + bit depth), due to rounding in the value of
            // svt_aom_eb_one_by_x[25-1].
            //
            // Thus we saturate so that, when z == 0, A[k] is set to 1 instead of 0.
            // This fixes the above issues (256 - A[k] fits in a uint8, and we can't
            // overflow), without significantly affecting the final result: z == 0
            // implies that the image is essentially "flat", so the local mean and
            // individual pixel values are very similar.
            //
            // Note that saturating on the other side, ie. requring A[k] <= 255,
            // would be a bad idea, as that corresponds to the case where the image
            // is very variable, when we want to preserve the local pixel value as
            // much as possible.
            A[k] = svt_aom_eb_x_by_xplus1[AOMMIN(z, 255)]; // in range [1, 256]

            // SGRPROJ_SGR - A[k] < 2^8 (from above), B[k] < 2^(bit_depth) * n,
            // svt_aom_eb_one_by_x[n - 1] = round(2^12 / n)
            // => the product here is < 2^(20 + bit_depth) <= 2^32,
            // and B[k] is set to a value < 2^(8 + bit depth)
            // This holds even with the rounding in svt_aom_eb_one_by_x and in the overall
            // result, as long as SGRPROJ_SGR - A[k] is strictly less than 2^8.
            B[k] = (int32_t)ROUND_POWER_OF_TWO(
                (uint32_t)(SGRPROJ_SGR - A[k]) * (uint32_t)B[k] * (uint32_t)svt_aom_eb_one_by_x[n - 1],
                SGRPROJ_RECIP_BITS);
        }
    }
    // Use the A[] and B[] arrays to calculate the filtered image
    assert(r == 2);
    for (i = 0; i < height; ++i) {
        if (!(i & 1)) { // even row
            for (j = 0; j < width; ++j) {
                const int32_t k  = i * buf_stride + j;
                const int32_t l  = i * dgd_stride + j;
                const int32_t m  = i * dst_stride + j;
                const int32_t nb = 5;
                const int32_t a  = (A[k - buf_stride] + A[k + buf_stride]) * 6 +
                    (A[k - 1 - buf_stride] + A[k - 1 + buf_stride] + A[k + 1 - buf_stride] + A[k + 1 + buf_stride]) * 5;
                const int32_t b = (B[k - buf_stride] + B[k + buf_stride]) * 6 +
                    (B[k - 1 - buf_stride] + B[k - 1 + buf_stride] + B[k + 1 - buf_stride] + B[k + 1 + buf_stride]) * 5;
                const int32_t v = a * dgd[l] + b;
                dst[m]          = ROUND_POWER_OF_TWO(v, SGRPROJ_SGR_BITS + nb - SGRPROJ_RST_BITS);
            }
        } else { // odd row
            for (j = 0; j < width; ++j) {
                const int32_t k  = i * buf_stride + j;
                const int32_t l  = i * dgd_stride + j;
                const int32_t m  = i * dst_stride + j;
                const int32_t nb = 4;
                const int32_t a  = A[k] * 6 + (A[k - 1] + A[k + 1]) * 5;
                const int32_t b  = B[k] * 6 + (B[k - 1] + B[k + 1]) * 5;
                const int32_t v  = a * dgd[l] + b;
                dst[m]           = ROUND_POWER_OF_TWO(v, SGRPROJ_SGR_BITS + nb - SGRPROJ_RST_BITS);
            }
        }
    }
}

static void selfguided_restoration_internal(int32_t *dgd, int32_t width, int32_t height, int32_t dgd_stride,
                                            int32_t *dst, int32_t dst_stride, int32_t bit_depth, int32_t sgr_params_idx,
                                            int32_t radius_idx) {
    const SgrParamsType *const params     = &svt_aom_eb_sgr_params[sgr_params_idx];
    const int32_t              r          = params->r[radius_idx];
    const int32_t              width_ext  = width + 2 * SGRPROJ_BORDER_HORZ;
    const int32_t              height_ext = height + 2 * SGRPROJ_BORDER_VERT;
    // Adjusting the stride of A and B here appears to avoid bad cache effects,
    // leading to a significant speed improvement.
    // We also align the stride to a multiple of 16 bytes, for consistency
    // with the SIMD version of this function.
    int32_t  buf_stride = ((width_ext + 3) & ~3) + 16;
    int32_t  a_[RESTORATION_PROC_UNIT_PELS];
    int32_t  b_[RESTORATION_PROC_UNIT_PELS];
    int32_t *A = a_;
    int32_t *B = b_;
    int32_t  i, j;

    assert(r <= MAX_RADIUS && "Need MAX_RADIUS >= r");
    assert(r <= SGRPROJ_BORDER_VERT - 1 && r <= SGRPROJ_BORDER_HORZ - 1 && "Need SGRPROJ_BORDER_* >= r+1");

    boxsum(dgd - dgd_stride * SGRPROJ_BORDER_VERT - SGRPROJ_BORDER_HORZ,
           width_ext,
           height_ext,
           dgd_stride,
           r,
           0,
           B,
           buf_stride);
    boxsum(dgd - dgd_stride * SGRPROJ_BORDER_VERT - SGRPROJ_BORDER_HORZ,
           width_ext,
           height_ext,
           dgd_stride,
           r,
           1,
           A,
           buf_stride);
    A += SGRPROJ_BORDER_VERT * buf_stride + SGRPROJ_BORDER_HORZ;
    B += SGRPROJ_BORDER_VERT * buf_stride + SGRPROJ_BORDER_HORZ;
    // Calculate the eventual A[] and B[] arrays. Include a 1-pixel border - ie,
    // for a 64x64 processing unit, we calculate 66x66 pixels of A[] and B[].
    for (i = -1; i < height + 1; ++i) {
        for (j = -1; j < width + 1; ++j) {
            const int32_t k = i * buf_stride + j;
            const int32_t n = (2 * r + 1) * (2 * r + 1);

            // a < 2^16 * n < 2^22 regardless of bit depth
            uint32_t a = ROUND_POWER_OF_TWO(A[k], 2 * (bit_depth - 8));
            // b < 2^8 * n < 2^14 regardless of bit depth
            uint32_t b = ROUND_POWER_OF_TWO(B[k], bit_depth - 8);

            // Each term in calculating p = a * n - b * b is < 2^16 * n^2 < 2^28,
            // and p itself satisfies p < 2^14 * n^2 < 2^26.
            // This bound on p is due to:
            // https://en.wikipedia.org/wiki/Popoviciu's_inequality_on_variances
            //
            // Note: Sometimes, in high bit depth, we can end up with a*n < b*b.
            // This is an artefact of rounding, and can only happen if all pixels
            // are (almost) identical, so in this case we saturate to p=0.
            uint32_t p = (a * n < b * b) ? 0 : a * n - b * b;

            const uint32_t s = params->s[radius_idx];

            // p * s < (2^14 * n^2) * round(2^20 / n^2 eps) < 2^34 / eps < 2^32
            // as long as eps >= 4. So p * s fits into a uint32_t, and z < 2^12
            // (this holds even after accounting for the rounding in s)
            const uint32_t z = ROUND_POWER_OF_TWO(p * s, SGRPROJ_MTABLE_BITS);

            // Note: We have to be quite careful about the value of A[k].
            // This is used as a blend factor between individual pixel values and the
            // local mean. So it logically has a range of [0, 256], including both
            // endpoints.
            //
            // This is a pain for hardware, as we'd like something which can be stored
            // in exactly 8 bits.
            // Further, in the calculation of B[k] below, if z == 0 and r == 2,
            // then A[k] "should be" 0. But then we can end up setting B[k] to a value
            // slightly above 2^(8 + bit depth), due to rounding in the value of
            // svt_aom_eb_one_by_x[25-1].
            //
            // Thus we saturate so that, when z == 0, A[k] is set to 1 instead of 0.
            // This fixes the above issues (256 - A[k] fits in a uint8, and we can't
            // overflow), without significantly affecting the final result: z == 0
            // implies that the image is essentially "flat", so the local mean and
            // individual pixel values are very similar.
            //
            // Note that saturating on the other side, ie. requring A[k] <= 255,
            // would be a bad idea, as that corresponds to the case where the image
            // is very variable, when we want to preserve the local pixel value as
            // much as possible.
            A[k] = svt_aom_eb_x_by_xplus1[AOMMIN(z, 255)]; // in range [1, 256]

            // SGRPROJ_SGR - A[k] < 2^8 (from above), B[k] < 2^(bit_depth) * n,
            // svt_aom_eb_one_by_x[n - 1] = round(2^12 / n)
            // => the product here is < 2^(20 + bit_depth) <= 2^32,
            // and B[k] is set to a value < 2^(8 + bit depth)
            // This holds even with the rounding in svt_aom_eb_one_by_x and in the overall
            // result, as long as SGRPROJ_SGR - A[k] is strictly less than 2^8.
            B[k] = (int32_t)ROUND_POWER_OF_TWO(
                (uint32_t)(SGRPROJ_SGR - A[k]) * (uint32_t)B[k] * (uint32_t)svt_aom_eb_one_by_x[n - 1],
                SGRPROJ_RECIP_BITS);
        }
    }
    // Use the A[] and B[] arrays to calculate the filtered image
    for (i = 0; i < height; ++i) {
        for (j = 0; j < width; ++j) {
            const int32_t k  = i * buf_stride + j;
            const int32_t l  = i * dgd_stride + j;
            const int32_t m  = i * dst_stride + j;
            const int32_t nb = 5;
            const int32_t a  = (A[k] + A[k - 1] + A[k + 1] + A[k - buf_stride] + A[k + buf_stride]) * 4 +
                (A[k - 1 - buf_stride] + A[k - 1 + buf_stride] + A[k + 1 - buf_stride] + A[k + 1 + buf_stride]) * 3;
            const int32_t b = (B[k] + B[k - 1] + B[k + 1] + B[k - buf_stride] + B[k + buf_stride]) * 4 +
                (B[k - 1 - buf_stride] + B[k - 1 + buf_stride] + B[k + 1 - buf_stride] + B[k + 1 + buf_stride]) * 3;
            const int32_t v = a * dgd[l] + b;
            dst[m]          = ROUND_POWER_OF_TWO(v, SGRPROJ_SGR_BITS + nb - SGRPROJ_RST_BITS);
        }
    }
}

void svt_av1_selfguided_restoration_c(const uint8_t *dgd8, int32_t width, int32_t height, int32_t dgd_stride,
                                      int32_t *flt0, int32_t *flt1, int32_t flt_stride, int32_t sgr_params_idx,
                                      int32_t bit_depth, int32_t highbd) {
    int32_t       dgd32_[RESTORATION_PROC_UNIT_PELS];
    const int32_t dgd32_stride = width + 2 * SGRPROJ_BORDER_HORZ;
    int32_t      *dgd32        = dgd32_ + dgd32_stride * SGRPROJ_BORDER_VERT + SGRPROJ_BORDER_HORZ;

    if (highbd) {
        const uint16_t *dgd16 = CONVERT_TO_SHORTPTR(dgd8);
        for (int32_t i = -SGRPROJ_BORDER_VERT; i < height + SGRPROJ_BORDER_VERT; ++i) {
            for (int32_t j = -SGRPROJ_BORDER_HORZ; j < width + SGRPROJ_BORDER_HORZ; ++j)
                dgd32[i * dgd32_stride + j] = dgd16[i * dgd_stride + j];
        }
    } else {
        for (int32_t i = -SGRPROJ_BORDER_VERT; i < height + SGRPROJ_BORDER_VERT; ++i) {
            for (int32_t j = -SGRPROJ_BORDER_HORZ; j < width + SGRPROJ_BORDER_HORZ; ++j)
                dgd32[i * dgd32_stride + j] = dgd8[i * dgd_stride + j];
        }
    }

    const SgrParamsType *const params = &svt_aom_eb_sgr_params[sgr_params_idx];
    // If params->r == 0 we skip the corresponding filter. We only allow one of
    // the radii to be 0, as having both equal to 0 would be equivalent to
    // skipping SGR entirely.
    assert(!(params->r[0] == 0 && params->r[1] == 0));

    if (params->r[0] > 0)
        selfguided_restoration_fast_internal(
            dgd32, width, height, dgd32_stride, flt0, flt_stride, bit_depth, sgr_params_idx, 0);
    if (params->r[1] > 0)
        selfguided_restoration_internal(
            dgd32, width, height, dgd32_stride, flt1, flt_stride, bit_depth, sgr_params_idx, 1);
}

void svt_apply_selfguided_restoration_c(const uint8_t *dat8, int32_t width, int32_t height, int32_t stride, int32_t eps,
                                        const int32_t *xqd, uint8_t *dst8, int32_t dst_stride, int32_t *tmpbuf,
                                        int32_t bit_depth, int32_t highbd) {
    int32_t *flt0 = tmpbuf;
    int32_t *flt1 = flt0 + RESTORATION_UNITPELS_MAX;
    assert(width * height <= RESTORATION_UNITPELS_MAX);

    svt_av1_selfguided_restoration_c(dat8, width, height, stride, flt0, flt1, width, eps, bit_depth, highbd);
    const SgrParamsType *const params = &svt_aom_eb_sgr_params[eps];
    int32_t                    xq[2];
    svt_decode_xq(xqd, xq, params);
    for (int32_t i = 0; i < height; ++i) {
        for (int32_t j = 0; j < width; ++j) {
            const int32_t  k      = i * width + j;
            uint8_t       *dst8ij = dst8 + i * dst_stride + j;
            const uint8_t *dat8ij = dat8 + i * stride + j;

            const uint16_t pre_u = highbd ? *CONVERT_TO_SHORTPTR(dat8ij) : *dat8ij;
            const int32_t  u     = (int32_t)pre_u << SGRPROJ_RST_BITS;
            int32_t        v     = u << SGRPROJ_PRJ_BITS;
            // If params->r == 0 then we skipped the filtering in
            // svt_av1_selfguided_restoration_c, i.e. flt[k] == u
            if (params->r[0] > 0)
                v += xq[0] * (flt0[k] - u);
            if (params->r[1] > 0)
                v += xq[1] * (flt1[k] - u);
            const int16_t w = (int16_t)ROUND_POWER_OF_TWO(v, SGRPROJ_PRJ_BITS + SGRPROJ_RST_BITS);

            const uint16_t out = clip_pixel_highbd(w, bit_depth);
            if (highbd)
                *CONVERT_TO_SHORTPTR(dst8ij) = out;
            else
                *dst8ij = (uint8_t)out;
        }
    }
}

void svt_aom_sgrproj_filter_stripe(const RestorationUnitInfo *rui, int32_t stripe_width, int32_t stripe_height,
                                   int32_t procunit_width, const uint8_t *src, int32_t src_stride, uint8_t *dst,
                                   int32_t dst_stride, int32_t *tmpbuf, int32_t bit_depth) {
    (void)bit_depth;
    assert(bit_depth == 8);

    for (int32_t j = 0; j < stripe_width; j += procunit_width) {
        int32_t w = AOMMIN(procunit_width, stripe_width - j);
        //CHKN SSE
        svt_apply_selfguided_restoration(src + j,
                                         w,
                                         stripe_height,
                                         src_stride,
                                         rui->sgrproj_info.ep,
                                         rui->sgrproj_info.xqd,
                                         dst + j,
                                         dst_stride,
                                         tmpbuf,
                                         bit_depth,
                                         0);
    }
}

void svt_aom_wiener_filter_stripe_highbd(const RestorationUnitInfo *rui, int32_t stripe_width, int32_t stripe_height,
                                         int32_t procunit_width, const uint8_t *src8, int32_t src_stride, uint8_t *dst8,
                                         int32_t dst_stride, int32_t *tmpbuf, int32_t bit_depth) {
    (void)tmpbuf;
    const ConvolveParams conv_params = get_conv_params_wiener(bit_depth);

    for (int32_t j = 0; j < stripe_width; j += procunit_width) {
        int32_t        w      = AOMMIN(procunit_width, (stripe_width - j + 15) & ~15);
        const uint8_t *src8_p = src8 + j;
        uint8_t       *dst8_p = dst8 + j;
        svt_av1_highbd_wiener_convolve_add_src(src8_p,
                                               src_stride,
                                               dst8_p,
                                               dst_stride, //CHKN  SSE
                                               rui->wiener_info.hfilter,
                                               rui->wiener_info.vfilter,
                                               w,
                                               stripe_height,
                                               &conv_params,
                                               bit_depth);
    }
}

void svt_aom_sgrproj_filter_stripe_highbd(const RestorationUnitInfo *rui, int32_t stripe_width, int32_t stripe_height,
                                          int32_t procunit_width, const uint8_t *src8, int32_t src_stride,
                                          uint8_t *dst8, int32_t dst_stride, int32_t *tmpbuf, int32_t bit_depth) {
    for (int32_t j = 0; j < stripe_width; j += procunit_width) {
        int32_t w = AOMMIN(procunit_width, stripe_width - j);

        //CHKN SSE
        svt_apply_selfguided_restoration(src8 + j,
                                         w,
                                         stripe_height,
                                         src_stride,
                                         rui->sgrproj_info.ep,
                                         rui->sgrproj_info.xqd,
                                         dst8 + j,
                                         dst_stride,
                                         tmpbuf,
                                         bit_depth,
                                         1);
    }
}

const StripeFilterFun svt_aom_stripe_filters[NUM_STRIPE_FILTERS] = {svt_aom_wiener_filter_stripe,
                                                                    svt_aom_sgrproj_filter_stripe,
                                                                    svt_aom_wiener_filter_stripe_highbd,
                                                                    svt_aom_sgrproj_filter_stripe_highbd};

// Filter one restoration unit
void svt_av1_loop_restoration_filter_unit(uint8_t need_bounadaries, const RestorationTileLimits *limits,
                                          const RestorationUnitInfo *rui, const RestorationStripeBoundaries *rsb,
                                          RestorationLineBuffers *rlbs, const Av1PixelRect *tile_rect,
                                          int32_t tile_stripe0, int32_t ss_x, int32_t ss_y, int32_t highbd,
                                          int32_t bit_depth, uint8_t *data8, int32_t stride, uint8_t *dst8,
                                          int32_t dst_stride, int32_t *tmpbuf, int32_t optimized_lr) {
    RestorationType unit_rtype = rui->restoration_type;

    int32_t  unit_h   = limits->v_end - limits->v_start;
    int32_t  unit_w   = limits->h_end - limits->h_start;
    uint8_t *data8_tl = data8 + limits->v_start * stride + limits->h_start;
    uint8_t *dst8_tl  = dst8 + limits->v_start * dst_stride + limits->h_start;

    if (unit_rtype == RESTORE_NONE) {
        svt_aom_copy_tile(unit_w, unit_h, data8_tl, stride, dst8_tl, dst_stride, highbd);
        return;
    }

    const int32_t filter_idx = 2 * highbd + (unit_rtype == RESTORE_SGRPROJ);
    assert(filter_idx < NUM_STRIPE_FILTERS);
    const StripeFilterFun stripe_filter = svt_aom_stripe_filters[filter_idx];

    const int32_t procunit_width = RESTORATION_PROC_UNIT_SIZE >> ss_x;

    // Convolve the whole tile one stripe at a time
    RestorationTileLimits remaining_stripes = *limits;
    int32_t               i                 = 0;
    while (i < unit_h) {
        int32_t copy_above, copy_below;
        remaining_stripes.v_start = limits->v_start + i;

        svt_aom_get_stripe_boundary_info(&remaining_stripes, tile_rect, ss_y, &copy_above, &copy_below);

        const int32_t full_stripe_height = RESTORATION_PROC_UNIT_SIZE >> ss_y;
        const int32_t runit_offset       = RESTORATION_UNIT_OFFSET >> ss_y;

        // Work out where this stripe's boundaries are within
        // rsb->stripe_boundary_{above,below}
        const int32_t tile_stripe  = (remaining_stripes.v_start - tile_rect->top + runit_offset) / full_stripe_height;
        const int32_t frame_stripe = tile_stripe0 + tile_stripe;
        const int32_t rsb_row      = RESTORATION_CTX_VERT * frame_stripe;

        // Calculate this stripe's height, based on two rules:
        // * The topmost stripe in each tile is 8 luma pixels shorter than usual.
        // * We can't extend past the end of the current restoration unit
        const int32_t nominal_stripe_height = full_stripe_height - ((tile_stripe == 0) ? runit_offset : 0);
        const int32_t h = AOMMIN(nominal_stripe_height, remaining_stripes.v_end - remaining_stripes.v_start);

        if (need_bounadaries)
            svt_aom_setup_processing_stripe_boundary(
                &remaining_stripes, rsb, rsb_row, highbd, h, data8, stride, rlbs, copy_above, copy_below, optimized_lr);

        stripe_filter(rui,
                      unit_w,
                      h,
                      procunit_width,
                      data8_tl + i * stride,
                      stride,
                      dst8_tl + i * dst_stride,
                      dst_stride,
                      tmpbuf,
                      bit_depth);
        if (need_bounadaries)
            svt_aom_restore_processing_stripe_boundary(
                &remaining_stripes, rlbs, highbd, h, data8, stride, copy_above, copy_below, optimized_lr);

        i += h;
    }
}

typedef struct {
    const RestorationInfo  *rsi;
    RestorationLineBuffers *rlbs;
    const Av1Common        *cm;
    int32_t                 tile_stripe0;
    int32_t                 ss_x, ss_y;
    int32_t                 highbd, bit_depth;
    uint8_t                *data8, *dst8;
    int32_t                 data_stride, dst_stride;
    int32_t                *tmpbuf;
} FilterFrameCtxt;

static void filter_frame_on_tile(int32_t tile_row, int32_t tile_col, void *priv) {
    (void)tile_col;
    FilterFrameCtxt *ctxt = (FilterFrameCtxt *)priv;
    ctxt->tile_stripe0    = (tile_row == 0) ? 0 : ctxt->cm->child_pcs->rst_end_stripe[tile_row - 1];
}

static void filter_frame_on_unit(const RestorationTileLimits *limits, const Av1PixelRect *tile_rect,
                                 int32_t rest_unit_idx, void *priv) {
    FilterFrameCtxt       *ctxt = (FilterFrameCtxt *)priv;
    const RestorationInfo *rsi  = ctxt->rsi;

    svt_av1_loop_restoration_filter_unit(1,
                                         limits,
                                         &rsi->unit_info[rest_unit_idx],
                                         &rsi->boundaries,
                                         ctxt->rlbs,
                                         tile_rect,
                                         ctxt->tile_stripe0,
                                         ctxt->ss_x,
                                         ctxt->ss_y,
                                         ctxt->highbd,
                                         ctxt->bit_depth,
                                         ctxt->data8,
                                         ctxt->data_stride,
                                         ctxt->dst8,
                                         ctxt->dst_stride,
                                         ctxt->tmpbuf,
                                         rsi->optimized_lr);
}

void svt_av1_loop_restoration_filter_frame(int32_t *rst_tmpbuf, Yv12BufferConfig *frame, Av1Common *cm,
                                           int32_t optimized_lr) {
    // assert(!cm->all_lossless);
    const int32_t num_planes = 3; // av1_num_planes(cm);
    typedef void (*CopyFun)(const Yv12BufferConfig *src, Yv12BufferConfig *dst);
    static const CopyFun copy_funs[3] = {
        svt_aom_yv12_copy_y_c, svt_aom_yv12_copy_u_c, svt_aom_yv12_copy_v_c}; //CHKN SSE

    Yv12BufferConfig *dst = &cm->rst_frame;

    const int32_t frame_width  = frame->crop_widths[0];
    const int32_t frame_height = frame->crop_heights[0];
    if (svt_aom_realloc_frame_buffer(dst,
                                     frame_width,
                                     frame_height,
                                     cm->subsampling_x,
                                     cm->subsampling_y,
                                     cm->use_highbitdepth,
                                     AOM_RESTORATION_FRAME_BORDER,
                                     cm->byte_alignment,
                                     NULL,
                                     NULL,
                                     NULL) < 0)
        SVT_LOG("Failed to allocate restoration dst buffer\n");

    RestorationLineBuffers rlbs;
    const int32_t          bit_depth = cm->bit_depth;
    const int32_t          highbd    = cm->use_highbitdepth;

    for (int32_t plane = 0; plane < num_planes; ++plane) {
        RestorationInfo *rsi   = &cm->child_pcs->rst_info[plane];
        RestorationType  rtype = rsi->frame_restoration_type;
        rsi->optimized_lr      = optimized_lr;

        if (rtype == RESTORE_NONE)
            continue;
        const int32_t is_uv        = plane > 0;
        const int32_t plane_width  = frame->crop_widths[is_uv];
        const int32_t plane_height = frame->crop_heights[is_uv];

        svt_extend_frame(frame->buffers[plane],
                         plane_width,
                         plane_height,
                         frame->strides[is_uv],
                         RESTORATION_BORDER,
                         RESTORATION_BORDER,
                         highbd);

        FilterFrameCtxt ctxt;
        ctxt.rsi         = rsi;
        ctxt.rlbs        = &rlbs;
        ctxt.cm          = cm;
        ctxt.ss_x        = is_uv && cm->subsampling_x;
        ctxt.ss_y        = is_uv && cm->subsampling_y;
        ctxt.highbd      = highbd;
        ctxt.bit_depth   = bit_depth;
        ctxt.data8       = frame->buffers[plane];
        ctxt.dst8        = dst->buffers[plane];
        ctxt.data_stride = frame->strides[is_uv];
        ctxt.dst_stride  = dst->strides[is_uv];
        ctxt.tmpbuf      = rst_tmpbuf;
        svt_aom_foreach_rest_unit_in_frame(cm, plane, filter_frame_on_tile, filter_frame_on_unit, &ctxt);

        copy_funs[plane](dst, frame);
    }
    if (dst->buffer_alloc_sz) {
        dst->buffer_alloc_sz = 0;
        EB_FREE_ARRAY(dst->buffer_alloc);
    }
}

static void foreach_rest_unit_in_tile(const Av1PixelRect *tile_rect, int32_t tile_row, int32_t tile_col,
                                      int32_t tile_cols, int32_t hunits_per_tile, int32_t units_per_tile,
                                      int32_t unit_size, int32_t ss_y, RestUnitVisitor on_rest_unit, void *priv) {
    const int32_t tile_w   = tile_rect->right - tile_rect->left;
    const int32_t tile_h   = tile_rect->bottom - tile_rect->top;
    const int32_t ext_size = unit_size * 3 / 2;

    const int32_t tile_idx  = tile_col + tile_row * tile_cols;
    const int32_t unit_idx0 = tile_idx * units_per_tile;

    int32_t y0 = 0, i = 0;
    while (y0 < tile_h) {
        int32_t remaining_h = tile_h - y0;
        int32_t h           = (remaining_h < ext_size) ? remaining_h : unit_size;

        RestorationTileLimits limits;
        limits.v_start = tile_rect->top + y0;
        limits.v_end   = tile_rect->top + y0 + h;
        assert(limits.v_end <= tile_rect->bottom);
        // Offset the tile upwards to align with the restoration processing stripe
        const int32_t voffset = RESTORATION_UNIT_OFFSET >> ss_y;
        limits.v_start        = AOMMAX(tile_rect->top, limits.v_start - voffset);
        if (limits.v_end < tile_rect->bottom)
            limits.v_end -= voffset;

        int32_t x0 = 0, j = 0;
        while (x0 < tile_w) {
            int32_t remaining_w = tile_w - x0;
            int32_t w           = (remaining_w < ext_size) ? remaining_w : unit_size;

            limits.h_start = tile_rect->left + x0;
            limits.h_end   = tile_rect->left + x0 + w;
            assert(limits.h_end <= tile_rect->right);

            const int32_t unit_idx = unit_idx0 + i * hunits_per_tile + j;
            on_rest_unit(&limits, tile_rect, unit_idx, priv);

            x0 += w;
            ++j;
        }

        y0 += h;
        ++i;
    }
}

void svt_aom_foreach_rest_unit_in_frame(Av1Common *cm, int32_t plane, RestTileStartVisitor on_tile,
                                        RestUnitVisitor on_rest_unit, void *priv) {
    const int32_t is_uv = plane > 0;
    const int32_t ss_y  = is_uv && cm->subsampling_y;

    const RestorationInfo *rsi = &cm->child_pcs->rst_info[plane];

    const Av1PixelRect tile_rect = svt_aom_whole_frame_rect(&cm->frm_size, cm->subsampling_x, cm->subsampling_y, is_uv);

    if (on_tile)
        on_tile(0, 0, priv);

    foreach_rest_unit_in_tile(&tile_rect,
                              0,
                              0,
                              1,
                              rsi->horz_units_per_tile,
                              rsi->units_per_tile,
                              rsi->restoration_unit_size,
                              ss_y,
                              on_rest_unit,
                              priv);
}
static void foreach_rest_unit_in_tile_seg(const Av1PixelRect *tile_rect, int32_t tile_row, int32_t tile_col,
                                          int32_t tile_cols, int32_t hunits_per_tile, int32_t units_per_tile,
                                          int32_t unit_size, int32_t ss_y, RestUnitVisitor on_rest_unit, void *priv,
                                          int32_t vunits_per_tile, uint8_t rest_segments_column_count,
                                          uint8_t rest_segments_row_count, uint32_t segment_index) {
    //tile_row=0
    //tile_col=0
    //tile_cols=1
    const int32_t tile_w   = tile_rect->right - tile_rect->left; // eq to pic_width
    const int32_t tile_h   = tile_rect->bottom - tile_rect->top; // eq to pic_height
    const int32_t ext_size = unit_size * 3 / 2;

    const int32_t tile_idx  = tile_col + tile_row * tile_cols; //eq to 0
    const int32_t unit_idx0 = tile_idx * units_per_tile; //eq to 0

    uint32_t x_seg_idx;
    uint32_t y_seg_idx;
    uint32_t picture_width_in_units  = hunits_per_tile;
    uint32_t picture_height_in_units = vunits_per_tile;
    SEGMENT_CONVERT_IDX_TO_XY(segment_index, x_seg_idx, y_seg_idx, rest_segments_column_count);
    uint32_t x_unit_start_idx = SEGMENT_START_IDX(x_seg_idx, picture_width_in_units, rest_segments_column_count);
    uint32_t x_unit_end_idx   = SEGMENT_END_IDX(x_seg_idx, picture_width_in_units, rest_segments_column_count);
    uint32_t y_unit_start_idx = SEGMENT_START_IDX(y_seg_idx, picture_height_in_units, rest_segments_row_count);
    uint32_t y_unit_end_idx   = SEGMENT_END_IDX(y_seg_idx, picture_height_in_units, rest_segments_row_count);

    int32_t y0   = y_unit_start_idx * unit_size;
    int32_t yend = ((int32_t)y_unit_end_idx == (int32_t)picture_height_in_units)
        ? tile_h
        : (int32_t)y_unit_end_idx * (int32_t)unit_size; //MIN(y_unit_end_idx * unit_size , tile_h);
    int32_t i    = y_unit_start_idx;

    while (y0 < yend) {
        int32_t remaining_h = tile_h - y0;
        int32_t h           = (remaining_h < ext_size)
                      ? remaining_h
                      : unit_size; //the area at the pic boundary should have size>= half unit_size to be an independent unit.
        //if not, it will be added to the last complete unit, increasing its size to up to  3/2 unit_size.

        RestorationTileLimits limits;
        limits.v_start = tile_rect->top + y0;
        limits.v_end   = tile_rect->top + y0 + h;
        assert(limits.v_end <= tile_rect->bottom);
        // Offset the tile upwards to align with the restoration processing stripe
        const int32_t voffset = RESTORATION_UNIT_OFFSET >> ss_y;
        limits.v_start        = AOMMAX(tile_rect->top, limits.v_start - voffset);
        if (limits.v_end < tile_rect->bottom)
            limits.v_end -= voffset;

        int32_t x0 = x_unit_start_idx * unit_size;
        // for the superblock below-right. If we're at the bottom or right of the tile,
        // this restoration unit might not exist, in which case we'll clamp accordingly.
        int32_t xend = ((int32_t)x_unit_end_idx == (int32_t)picture_width_in_units)
            ? tile_w
            : AOMMIN((int32_t)x_unit_end_idx * (int32_t)unit_size, tile_w);
        int32_t j    = x_unit_start_idx;

        while (x0 < xend) {
            int32_t remaining_w = tile_w - x0;
            int32_t w           = (remaining_w < ext_size) ? remaining_w : unit_size;

            limits.h_start = tile_rect->left + x0;
            limits.h_end   = tile_rect->left + x0 + w;
            assert(limits.h_end <= tile_rect->right);

            const int32_t unit_idx = unit_idx0 + i * hunits_per_tile + j;
            on_rest_unit(&limits, tile_rect, unit_idx, priv);

            x0 += w;
            ++j;
        }

        y0 += h;
        ++i;
    }
}
/* For each restoration unit in the frame, get the best filter parameters and distortions
   for the passed filter type.
*/
void svt_aom_foreach_rest_unit_in_frame_seg(Av1Common *cm, int32_t plane, RestTileStartVisitor on_tile,
                                            RestUnitVisitor on_rest_unit, void *priv,
                                            uint8_t rest_segments_column_count, uint8_t rest_segments_row_count,
                                            uint32_t segment_index) {
    const int32_t is_uv = plane > 0;
    const int32_t ss_y  = is_uv && cm->subsampling_y;

    const RestorationInfo *rsi = &cm->child_pcs->rst_info[plane];

    const Av1PixelRect tile_rect = svt_aom_whole_frame_rect(&cm->frm_size, cm->subsampling_x, cm->subsampling_y, is_uv);

    if (on_tile)
        on_tile(0, 0, priv); //will set rsc->tile_strip0=0;

    foreach_rest_unit_in_tile_seg(&tile_rect,
                                  0,
                                  0,
                                  1,
                                  rsi->horz_units_per_tile,
                                  rsi->units_per_tile,
                                  rsi->restoration_unit_size,
                                  ss_y,
                                  on_rest_unit,
                                  priv,
                                  rsi->vert_units_per_tile,
                                  rest_segments_column_count,
                                  rest_segments_row_count,
                                  segment_index);
}

int32_t svt_av1_loop_restoration_corners_in_sb(Av1Common *cm, SeqHeader *seq_header_p, int32_t plane, int32_t mi_row,
                                               int32_t mi_col, BlockSize bsize, int32_t *rcol0, int32_t *rcol1,
                                               int32_t *rrow0, int32_t *rrow1, int32_t *tile_tl_idx) {
    assert(rcol0 && rcol1 && rrow0 && rrow1);
    if (bsize != seq_header_p->sb_size)
        return 0;
    if (cm->child_pcs->rst_info[plane].frame_restoration_type == RESTORE_NONE)
        return 0;

    // assert(!cm->all_lossless);

    const int32_t is_uv = plane > 0;

    const Av1PixelRect tile_rect = svt_aom_whole_frame_rect(&cm->frm_size, cm->subsampling_x, cm->subsampling_y, is_uv);
    const int32_t      tile_w    = tile_rect.right - tile_rect.left;
    const int32_t      tile_h    = tile_rect.bottom - tile_rect.top;

    const int32_t mi_top  = 0;
    const int32_t mi_left = 0;

    // Compute the mi-unit corners of the superblock relative to the top-left of
    // the tile
    const int32_t mi_rel_row0 = mi_row - mi_top;
    const int32_t mi_rel_col0 = mi_col - mi_left;
    const int32_t mi_rel_row1 = mi_rel_row0 + mi_size_high[bsize];
    const int32_t mi_rel_col1 = mi_rel_col0 + mi_size_wide[bsize];

    const RestorationInfo *rsi  = &cm->child_pcs->rst_info[plane];
    const int32_t          size = rsi->restoration_unit_size;

    // Calculate the number of restoration units in this tile (which might be
    // strictly less than rsi->horz_units_per_tile and rsi->vert_units_per_tile)
    const int32_t horz_units = count_units_in_tile(size, tile_w);
    const int32_t vert_units = count_units_in_tile(size, tile_h);

    // The size of an MI-unit on this plane of the image
    const int32_t ss_x      = is_uv && cm->subsampling_x;
    const int32_t ss_y      = is_uv && cm->subsampling_y;
    const int32_t mi_size_x = MI_SIZE >> ss_x;
    const int32_t mi_size_y = MI_SIZE >> ss_y;

    // Write m for the relative mi column or row, D for the superres denominator
    // and N for the superres numerator. If u is the upscaled (called "unscaled"
    // elsewhere) pixel offset then we can write the downscaled pixel offset in
    // two ways as:
    //
    //   MI_SIZE * m = N / D u
    //
    // from which we get u = D * MI_SIZE * m / N

    const int     mi_to_num_x = !av1_superres_unscaled(&cm->frm_size) ? mi_size_x * cm->frm_size.superres_denominator
                                                                      : mi_size_x;
    const int     mi_to_num_y = mi_size_y;
    const int     denom_x     = !av1_superres_unscaled(&cm->frm_size) ? size * SCALE_NUMERATOR : size;
    const int32_t denom_y     = size;

    const int32_t rnd_x = denom_x - 1;
    const int32_t rnd_y = denom_y - 1;

    // rcol0/rrow0 should be the first column/row of restoration units (relative
    // to the top-left of the tile) that doesn't start left/below of
    // mi_col/mi_row. For this calculation, we need to round up the division (if
    // the sb starts at runit column 10.1, the first matching runit has column
    // index 11)
    *rcol0 = (mi_rel_col0 * mi_to_num_x + rnd_x) / denom_x;
    *rrow0 = (mi_rel_row0 * mi_to_num_y + rnd_y) / denom_y;

    // rel_col1/rel_row1 is the equivalent calculation, but for the superblock
    // below-right. If we're at the bottom or right of the tile, this restoration
    // unit might not exist, in which case we'll clamp accordingly.
    *rcol1 = AOMMIN((mi_rel_col1 * mi_to_num_x + rnd_x) / denom_x, horz_units);
    *rrow1 = AOMMIN((mi_rel_row1 * mi_to_num_y + rnd_y) / denom_y, vert_units);

    const int32_t tile_idx = 0;
    *tile_tl_idx           = tile_idx * rsi->units_per_tile;

    return *rcol0 < *rcol1 && *rrow0 < *rrow1;
}

// Extend to left and right
void svt_aom_extend_lines(uint8_t *buf, int32_t width, int32_t height, int32_t stride, int32_t extend,
                          int32_t use_highbitdepth) {
    for (int32_t i = 0; i < height; ++i) {
        if (use_highbitdepth) {
            uint16_t *buf16 = (uint16_t *)buf;
            svt_aom_memset16(buf16 - extend, buf16[0], extend);
            svt_aom_memset16(buf16 + width, buf16[width - 1], extend);
        } else {
            memset(buf - extend, buf[0], extend);
            memset(buf + width, buf[width - 1], extend);
        }
        buf += stride;
    }
}

void svt_aom_save_deblock_boundary_lines(uint8_t *src_buf, int32_t src_stride, int32_t src_width, int32_t src_height,
                                         const Av1Common *cm, int32_t plane, int32_t row, int32_t stripe,
                                         int32_t use_highbd, int32_t is_above,
                                         RestorationStripeBoundaries *boundaries) {
    const int32_t is_uv     = plane > 0;
    src_stride              = src_stride << use_highbd;
    const uint8_t *src_rows = src_buf + row * src_stride;

    uint8_t      *bdry_buf    = is_above ? boundaries->stripe_boundary_above : boundaries->stripe_boundary_below;
    uint8_t      *bdry_start  = bdry_buf + (RESTORATION_EXTRA_HORZ << use_highbd);
    const int32_t bdry_stride = boundaries->stripe_boundary_stride << use_highbd;
    uint8_t      *bdry_rows   = bdry_start + RESTORATION_CTX_VERT * stripe * bdry_stride;

    // There is a rare case in which a processing stripe can end 1px above the
    // crop border. In this case, we do want to use deblocked pixels from below
    // the stripe (hence why we ended up in this function), but instead of
    // fetching 2 "below" rows we need to fetch one and duplicate it.
    // This is equivalent to clamping the sample locations against the crop border
    const int32_t lines_to_save = AOMMIN(RESTORATION_CTX_VERT, src_height - row);

    assert(lines_to_save == 1 || lines_to_save == 2);

    int32_t upscaled_width;
    int32_t line_bytes;

    if (!av1_superres_unscaled(&cm->frm_size)) {
        int32_t sx     = is_uv && cm->subsampling_x;
        upscaled_width = (cm->frm_size.superres_upscaled_width + sx) >> sx;
        line_bytes     = upscaled_width << use_highbd;

        svt_av1_upscale_normative_rows(cm,
                                       (src_rows),
                                       src_stride >> use_highbd,
                                       (bdry_rows),
                                       boundaries->stripe_boundary_stride,
                                       lines_to_save,
                                       sx,
                                       cm->bit_depth,
                                       use_highbd);
    } else {
        upscaled_width = src_width;
        line_bytes     = upscaled_width << use_highbd;
        for (int32_t i = 0; i < lines_to_save; i++) {
            svt_memcpy(bdry_rows + i * bdry_stride, src_rows + i * src_stride, line_bytes);
        }
    }
    // If we only saved one line, then copy it into the second line buffer
    if (lines_to_save == 1)
        svt_memcpy(bdry_rows + bdry_stride, bdry_rows, line_bytes);

    svt_aom_extend_lines(
        bdry_rows, upscaled_width, RESTORATION_CTX_VERT, bdry_stride, RESTORATION_EXTRA_HORZ, use_highbd);
}

void svt_aom_save_cdef_boundary_lines(uint8_t *src_buf, int32_t src_stride, int32_t src_width, const Av1Common *cm,
                                      int32_t plane, int32_t row, int32_t stripe, int32_t use_highbd, int32_t is_above,
                                      RestorationStripeBoundaries *boundaries) {
    const int32_t is_uv     = plane > 0;
    src_stride              = src_stride << use_highbd;
    const uint8_t *src_rows = src_buf + row * src_stride;

    uint8_t      *bdry_buf    = is_above ? boundaries->stripe_boundary_above : boundaries->stripe_boundary_below;
    uint8_t      *bdry_start  = bdry_buf + (RESTORATION_EXTRA_HORZ << use_highbd);
    const int32_t bdry_stride = boundaries->stripe_boundary_stride << use_highbd;
    uint8_t      *bdry_rows   = bdry_start + RESTORATION_CTX_VERT * stripe * bdry_stride;

    // At the point where this function is called, we've already applied
    // superres. So we don't need to extend the lines here, we can just
    // pull directly from the topmost row of the upscaled frame.
    const int32_t ss_x           = is_uv && cm->subsampling_x;
    const int32_t upscaled_width = av1_superres_unscaled(&cm->frm_size)
        ? src_width
        : (cm->frm_size.superres_upscaled_width + ss_x) >> ss_x;
    const int32_t line_bytes     = upscaled_width << use_highbd;
    for (int32_t i = 0; i < RESTORATION_CTX_VERT; i++) {
        // Copy the line at 'row' into both context lines. This is because
        // we want to (effectively) extend the outermost row of CDEF data
        // from this tile to produce a border, rather than using deblocked
        // pixels from the tile above/below.
        svt_memcpy(bdry_rows + i * bdry_stride, src_rows, line_bytes);
    }
    svt_aom_extend_lines(
        bdry_rows, upscaled_width, RESTORATION_CTX_VERT, bdry_stride, RESTORATION_EXTRA_HORZ, use_highbd);
}

void svt_aom_save_tile_row_boundary_lines(uint8_t *src, int32_t src_stride, int32_t src_width, int32_t src_height,
                                          int32_t use_highbd, int32_t plane, Av1Common *cm, int32_t after_cdef,
                                          RestorationStripeBoundaries *boundaries) {
    const int32_t is_uv         = plane > 0;
    const int32_t ss_y          = is_uv && cm->subsampling_y;
    const int32_t stripe_height = RESTORATION_PROC_UNIT_SIZE >> ss_y;
    const int32_t stripe_off    = RESTORATION_UNIT_OFFSET >> ss_y;

    // Get the tile rectangle, with height rounded up to the next multiple of 8
    // luma pixels (only relevant for the bottom tile of the frame)
    const Av1PixelRect tile_rect = svt_aom_whole_frame_rect(&cm->frm_size, cm->subsampling_x, cm->subsampling_y, is_uv);
    const int32_t      stripe0   = 0;

    int32_t plane_height = ROUND_POWER_OF_TWO(cm->frm_size.frame_height, ss_y);

    int32_t tile_stripe;
    for (tile_stripe = 0;; ++tile_stripe) {
        const int32_t rel_y0 = AOMMAX(0, tile_stripe * stripe_height - stripe_off);
        const int32_t y0     = tile_rect.top + rel_y0;
        if (y0 >= tile_rect.bottom)
            break;

        const int32_t rel_y1 = (tile_stripe + 1) * stripe_height - stripe_off;
        const int32_t y1     = AOMMIN(tile_rect.top + rel_y1, tile_rect.bottom);

        const int32_t frame_stripe = stripe0 + tile_stripe;

        int32_t use_deblock_above, use_deblock_below;
        // In this case, we should only use CDEF pixels at the top
        // and bottom of the frame as a whole; internal tile boundaries
        // can use deblocked pixels from adjacent tiles for context.
        use_deblock_above = (frame_stripe > 0);
        use_deblock_below = (y1 < plane_height);

        if (!after_cdef) {
            // Save deblocked context where needed.
            if (use_deblock_above) {
                svt_aom_save_deblock_boundary_lines(src,
                                                    src_stride,
                                                    src_width,
                                                    src_height,
                                                    cm,
                                                    plane,
                                                    y0 - RESTORATION_CTX_VERT,
                                                    frame_stripe,
                                                    use_highbd,
                                                    1,
                                                    boundaries);
            }
            if (use_deblock_below) {
                svt_aom_save_deblock_boundary_lines(
                    src, src_stride, src_width, src_height, cm, plane, y1, frame_stripe, use_highbd, 0, boundaries);
            }
        } else {
            // Save CDEF context where needed. Note that we need to save the CDEF
            // context for a particular boundary iff we *didn't* save deblocked
            // context for that boundary.
            //
            // In addition, we need to save copies of the outermost line within
            // the tile, rather than using data from outside the tile.
            if (!use_deblock_above) {
                svt_aom_save_cdef_boundary_lines(
                    src, src_stride, src_width, cm, plane, y0, frame_stripe, use_highbd, 1, boundaries);
            }
            if (!use_deblock_below) {
                svt_aom_save_cdef_boundary_lines(
                    src, src_stride, src_width, cm, plane, y1 - 1, frame_stripe, use_highbd, 0, boundaries);
            }
        }
    }
}

// For each RESTORATION_PROC_UNIT_SIZE pixel high stripe, save 4 scan
// lines to be used as boundary in the loop restoration process. The
// lines are saved in rst_internal.stripe_boundary_lines
void svt_av1_loop_restoration_save_boundary_lines(const Yv12BufferConfig *frame, Av1Common *cm, int32_t after_cdef) {
    const int32_t num_planes = 3; // av1_num_planes(cm);
    const int32_t use_highbd = cm->use_highbitdepth;

    for (int32_t p = 0; p < num_planes; ++p) {
        const int32_t                is_uv       = p > 0;
        int32_t                      crop_width  = frame->crop_widths[is_uv];
        int32_t                      crop_height = frame->crop_heights[is_uv];
        uint8_t                     *src_buf     = REAL_PTR(use_highbd, frame->buffers[p]);
        int32_t                      src_stride  = frame->strides[is_uv];
        RestorationStripeBoundaries *boundaries  = &cm->child_pcs->rst_info[p].boundaries;

        svt_aom_save_tile_row_boundary_lines(
            src_buf, src_stride, crop_width, crop_height, use_highbd, p, cm, after_cdef, boundaries);
    }
}

// Assumes cm->rst_info[p].restoration_unit_size is already initialized
EbErrorType svt_av1_alloc_restoration_buffers(PictureControlSet *pcs, Av1Common *cm) {
    EbErrorType   return_error = EB_ErrorNone;
    const int32_t num_planes   = 3; // av1_num_planes(cm);
    for (int32_t p = 0; p < num_planes; ++p)
        return_error = svt_av1_alloc_restoration_struct(cm, &pcs->rst_info[p], p > 0);

    // For striped loop restoration, we divide each row of tiles into "stripes",
    // of height 64 luma pixels but with an offset by RESTORATION_UNIT_OFFSET
    // luma pixels to match the output from CDEF. We will need to store 2 *
    // RESTORATION_CTX_VERT lines of data for each stripe, and also need to be
    // able to quickly answer the question "Where is the <n>'th stripe for tile
    // row <m>?" To make that efficient, we generate the rst_last_stripe array.
    int32_t num_stripes = 0;
    for (int32_t i = 0; i < 1 /*cm->tile_rows*/; ++i) {
        //TileInfo tile_info;
        //svt_av1_tile_set_row(&tile_info, cm, i);

        const int32_t mi_h         = cm->mi_rows; // tile_info.mi_row_end - tile_info.mi_row_start;
        const int32_t ext_h        = RESTORATION_UNIT_OFFSET + (mi_h << MI_SIZE_LOG2);
        const int32_t tile_stripes = (ext_h + 63) / 64;
        num_stripes += tile_stripes;
        pcs->rst_end_stripe[i] = num_stripes;
    }

    // Now we need to allocate enough space to store the line buffers for the
    // stripes
    const int32_t frame_w = cm->frm_size.superres_upscaled_width;

    for (int32_t p = 0; p < num_planes; ++p) {
        const int32_t                is_uv      = p > 0;
        const int32_t                ss_x       = is_uv && cm->subsampling_x;
        const int32_t                plane_w    = ((frame_w + ss_x) >> ss_x) + 2 * RESTORATION_EXTRA_HORZ;
        const int32_t                stride     = ALIGN_POWER_OF_TWO(plane_w, 5);
        const int32_t                buf_size   = num_stripes * stride * RESTORATION_CTX_VERT << 1;
        RestorationStripeBoundaries *boundaries = &pcs->rst_info[p].boundaries;

        {
            EB_MALLOC(boundaries->stripe_boundary_above, buf_size);
            EB_MALLOC(boundaries->stripe_boundary_below, buf_size);

            boundaries->stripe_boundary_size = buf_size;
        }
        boundaries->stripe_boundary_stride = stride;
    }

    return return_error;
}
