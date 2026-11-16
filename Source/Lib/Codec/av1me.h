/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
 */

#ifndef AOM_AV1_ENCODER_ME_H_
#define AOM_AV1_ENCODER_ME_H_

#include "definitions.h"
#include "coding_unit.h"
#include "inter_prediction.h"
#include "warped_motion.h"

#ifdef __cplusplus
extern "C" {
#endif

// The maximum number of steps in a step search given the largest
// allowed initial step
#define MAX_MVSEARCH_STEPS 11
// Max full pel mv specified in the unit of full pixel
// Enable the use of motion vector in range [-1023, 1023].
#define MAX_FULL_PEL_VAL ((1 << (MAX_MVSEARCH_STEPS - 1)) - 1)
// Maximum size of the first step in full pel units
#define MAX_FIRST_STEP (1 << (MAX_MVSEARCH_STEPS - 1))

static INLINE void svt_av1_constrain_mv_limits_to_tile(MvLimits* mv_limits, const TileInfo* tile, int mi_row,
                                                       int mi_col, int block_width, int block_height, int frame_width,
                                                       int frame_height) {
    const int right    = AOMMIN(tile->mi_col_end * MI_SIZE, frame_width);
    const int bottom   = AOMMIN(tile->mi_row_end * MI_SIZE, frame_height);
    mv_limits->col_min = AOMMAX(mv_limits->col_min, (tile->mi_col_start - mi_col) * MI_SIZE);
    mv_limits->col_max = AOMMIN(mv_limits->col_max, AOMMAX(0, right - mi_col * MI_SIZE - block_width));
    mv_limits->row_min = AOMMAX(mv_limits->row_min, (tile->mi_row_start - mi_row) * MI_SIZE);
    mv_limits->row_max = AOMMIN(mv_limits->row_max, AOMMAX(0, bottom - mi_row * MI_SIZE - block_height));
}

static INLINE bool svt_av1_is_mv_predictor_tile_safe(const MacroBlockD* xd, Mv mv) {
    // NEWMV deltas must survive the decoder's frame-relative reference-MV clamp after extraction.
    const int left   = (xd->tile.mi_col_start - xd->mi_col - xd->n8_w) * MI_SIZE * 8 - MV_BORDER;
    const int right  = (xd->tile.mi_col_end - xd->mi_col) * MI_SIZE * 8 + MV_BORDER;
    const int top    = (xd->tile.mi_row_start - xd->mi_row - xd->n8_h) * MI_SIZE * 8 - MV_BORDER;
    const int bottom = (xd->tile.mi_row_end - xd->mi_row) * MI_SIZE * 8 + MV_BORDER;
    return mv.x >= left && mv.x <= right && mv.y >= top && mv.y <= bottom;
}

static INLINE bool svt_av1_is_interp_axis_tile_safe(int origin, int size, int filter_size, int mv, int subsampling,
                                                    int tile_start, int tile_end, InterpFilter filter) {
    const int shift        = 3 + subsampling;
    const int phase        = (mv & ((1 << shift) - 1)) << (1 - subsampling);
    int       first        = 0;
    int       last         = 0;
    const int first_filter = filter == SWITCHABLE ? EIGHTTAP_REGULAR : filter;
    const int last_filter  = filter == SWITCHABLE ? MULTITAP_SHARP : filter;
    for (int f = first_filter; f <= last_filter; ++f) {
        const InterpFilterParams params = av1_get_interp_filter_params_with_block_size((InterpFilter)f, filter_size);
        const int16_t*           kernel = av1_get_interp_filter_subpel_kernel(params, phase);
        for (int tap = 0; tap < params.taps; ++tap) {
            if (kernel[tap]) {
                first = AOMMIN(first, tap - (params.taps / 2 - 1));
                last  = AOMMAX(last, tap - (params.taps / 2 - 1));
            }
        }
    }
    const int start = origin + (mv >> shift);
    return start + first >= tile_start && start + size + last <= tile_end;
}

// MV components are in eighth-luma-pel units, including for 4:2:0 chroma.
// Check the union of switchable filters before interpolation search selects one.
static INLINE bool svt_av1_is_mv_tile_safe(const TileInfo* tile, Mv mv, int x, int y, int width, int height,
                                           int frame_width, int frame_height, InterpFilter filter) {
    for (int ss = 0; ss <= 1; ++ss) {
        const int px             = ss ? (x & ~7) >> 1 : x;
        const int py             = ss ? (y & ~7) >> 1 : y;
        const int pw             = ss ? AOMMAX(8, width) >> 1 : width;
        const int ph             = ss ? AOMMAX(8, height) >> 1 : height;
        const int right          = AOMMIN(tile->mi_col_end * MI_SIZE, frame_width) >> ss;
        const int bottom         = AOMMIN(tile->mi_row_end * MI_SIZE, frame_height) >> ss;
        const int visible_width  = AOMMIN(pw, (frame_width >> ss) - px);
        const int visible_height = AOMMIN(ph, (frame_height >> ss) - py);
        if (visible_width <= 0 || visible_height <= 0) {
            continue;
        }
        if (!svt_av1_is_interp_axis_tile_safe(
                px, visible_width, pw, mv.x, ss, (tile->mi_col_start * MI_SIZE) >> ss, right, filter) ||
            !svt_av1_is_interp_axis_tile_safe(
                py, visible_height, ph, mv.y, ss, (tile->mi_row_start * MI_SIZE) >> ss, bottom, filter)) {
            return false;
        }
    }
    return true;
}

static INLINE bool svt_av1_is_warp_tile_safe(const TileInfo* tile, const WarpedMotionParams* wm, int x, int y,
                                             int width, int height, int frame_width, int frame_height) {
    // warp_affine filters a 15x15 source envelope around each projected 8x8 centre.
    for (int ss = 0; ss <= 1; ++ss) {
        const int left   = (tile->mi_col_start * MI_SIZE) >> ss;
        const int top    = (tile->mi_row_start * MI_SIZE) >> ss;
        const int right  = AOMMIN(tile->mi_col_end * MI_SIZE, frame_width) >> ss;
        const int bottom = AOMMIN(tile->mi_row_end * MI_SIZE, frame_height) >> ss;
        for (int row = y >> ss; row < (y + height) >> ss; row += 8) {
            for (int col = x >> ss; col < (x + width) >> ss; col += 8) {
                const int64_t cx = (col + 4) << ss;
                const int64_t cy = (row + 4) << ss;
                const int     sx = (int)(((int64_t)wm->wmmat[2] * cx + wm->wmmat[3] * cy + wm->wmmat[0]) >>
                                         (WARPEDMODEL_PREC_BITS + ss));
                const int     sy = (int)(((int64_t)wm->wmmat[4] * cx + wm->wmmat[5] * cy + wm->wmmat[1]) >>
                                         (WARPEDMODEL_PREC_BITS + ss));
                if (sx - 7 < left || sx + 8 > right || sy - 7 < top || sy + 8 > bottom) {
                    return false;
                }
            }
        }
    }
    return true;
}

static INLINE bool svt_av1_is_local_warp_tile_invariant(const TileInfo* tile, const WarpedMotionParams* wm, Mv mv,
                                                        int x, int y, int width, int height) {
    const int64_t unit = (int64_t)1 << WARPEDMODEL_PREC_BITS;
    const int64_t left = tile->mi_col_start * MI_SIZE;
    const int64_t top  = tile->mi_row_start * MI_SIZE;
    const int64_t cx   = x - left + width / 2 - 1;
    const int64_t cy   = y - top + height / 2 - 1;
    const int64_t tx   = mv.x * (unit >> 3) - cx * (wm->wmmat[2] - unit) - cy * wm->wmmat[3];
    const int64_t ty   = mv.y * (unit >> 3) - cx * wm->wmmat[4] - cy * (wm->wmmat[5] - unit);
    // The local affine model is derived again by the decoder; its translation clamp must commute with relocation.
    return AOMMAX(-WARPEDMODEL_TRANS_CLAMP, AOMMIN(WARPEDMODEL_TRANS_CLAMP - 1, tx)) ==
        wm->wmmat[0] + left * (wm->wmmat[2] - unit) + top * wm->wmmat[3] &&
        AOMMAX(-WARPEDMODEL_TRANS_CLAMP, AOMMIN(WARPEDMODEL_TRANS_CLAMP - 1, ty)) ==
        wm->wmmat[1] + left * wm->wmmat[4] + top * (wm->wmmat[5] - unit);
}

// motion search site
typedef struct SearchSite {
    Mv  mv;
    int offset;
} SearchSite;

typedef struct SearchSiteConfig {
    SearchSite ss[8 * MAX_MVSEARCH_STEPS + 1];
    int        ss_count;
    int        searches_per_step;
} SearchSiteConfig;

typedef unsigned int (*AomObmcSadFn)(const uint8_t* pred, int pred_stride, const int32_t* wsrc, const int32_t* msk);
typedef unsigned int (*AomObmcVarianceFn)(const uint8_t* pred, int pred_stride, const int32_t* wsrc, const int32_t* msk,
                                          unsigned int* sse);
typedef unsigned int (*AomObmcSubpixvarianceFn)(const uint8_t* pred, int pred_stride, int xoffset, int yoffset,
                                                const int32_t* wsrc, const int32_t* msk, unsigned int* sse);
typedef unsigned int (*AomSadFn)(const uint8_t* a, int a_stride, const uint8_t* b, int b_stride);

typedef unsigned int (*AomVarianceFn)(const uint8_t* a, int a_stride, const uint8_t* b, int b_stride,
                                      unsigned int* sse);
typedef unsigned int (*AomSubpixVarianceFn)(const uint8_t* a, int a_stride, int xoffset, int yoffset, const uint8_t* b,
                                            int b_stride, unsigned int* sse);
typedef void (*AomSadMultiDFn)(const uint8_t* a, int a_stride, const uint8_t* const b_array[], int b_stride,
                               unsigned int* sad_array);

typedef struct aom_variance_vtable {
    AomSadFn                sdf;
    AomVarianceFn           vf;
    AomVarianceFn           vf_hbd_10;
    AomSubpixVarianceFn     svf;
    AomSadMultiDFn          sdx4df;
    AomObmcSadFn            osdf;
    AomObmcVarianceFn       ovf;
    AomObmcSubpixvarianceFn osvf;

} AomVarianceFnPtr;

extern AomVarianceFnPtr svt_aom_mefn_ptr[BLOCK_SIZES_ALL];

void av1_init_dsmotion_compensation(SearchSiteConfig* cfg, int stride);
void svt_av1_init3smotion_compensation(SearchSiteConfig* cfg, int stride);
void svt_av1_set_mv_search_range(MvLimits* mv_limits, const Mv mv);
int  svt_av1_full_pixel_search(struct PictureControlSet* pcs, IntraBcContext /*MACROBLOCK*/* x, BlockSize bsize,
                               Mv* mvp_full, int step_param, int error_per_bit, int* cost_list, const Mv ref_mv);
int  svt_aom_mv_err_cost(const Mv mv, const Mv ref, const int* mvjcost, const int* mvcost[2], int error_per_bit);
int  svt_aom_mv_err_cost_light(const Mv mv, const Mv ref);
int  svt_av1_get_mvpred_var(const IntraBcContext* x, const Mv best_mv, const Mv center_mv, const AomVarianceFnPtr* vfp,
                            int use_mvcost);
void svt_av1_intrabc_hash_search(struct PictureControlSet* pcs, IntraBcContext* x, BlockSize bsize, int x_pos,
                                 int y_pos, const Mv ref_mv, int intra, const AomVarianceFnPtr* fn_ptr,
                                 int* best_hash_cost, Mv* best_hash_mv);
int  svt_av1_intrabc_pred_first(IntraBcContext* x, BlockSize bsize, const Mv* mvp_full, uint16_t pred_exit_th,
                                Mv* best_pred_mv);
int  svt_av1_intrabc_pred_best4(IntraBcContext* x, BlockSize bsize, const Mv cands[4], uint16_t pred_exit_th, Mv* best);
void svt_av1_intrabc_probe_around(IntraBcContext* x, BlockSize bsize, Mv center, int npts, Mv* best);
#if CONFIG_ENABLE_OBMC
struct ModeDecisionContext;
struct Av1Common;
int svt_av1_obmc_full_pixel_search(struct ModeDecisionContext* ctx, IntraBcContext* x, const Mv mvp_full, int sadpb,
                                   const AomVarianceFnPtr* fn_ptr, const Mv ref_mv, Mv* dst_mv, int is_second);
int svt_av1_find_best_obmc_sub_pixel_tree_up(struct ModeDecisionContext* ctx, IntraBcContext* x,
                                             const struct Av1Common* const cm, int mi_row, int mi_col, Mv* bestmv,
                                             const Mv ref_mv, int allow_hp, int error_per_bit,
                                             const AomVarianceFnPtr* vfp, int forced_stop, int iters_per_step,
                                             int* mvjcost, const int* mvcost[2], int* distortion, unsigned int* sse1,
                                             int is_second, int use_accurate_subpel_search);
#endif
#ifdef __cplusplus
} // extern "C"
#endif

#endif // AOM_AV1_ENCODER_ME_H_
