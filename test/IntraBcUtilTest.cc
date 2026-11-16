/*
 * Copyright(c) 2019 Netflix, Inc.
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at https://www.aomedia.org/license/software-license. If the
 * Alliance for Open Media Patent License 1.0 was not distributed with this
 * source code in the PATENTS file, you can obtain it at
 * https://www.aomedia.org/license/patent-license.
 */

/******************************************************************************
 * @file IntraBcUtilTest.cc
 *
 * @brief Unit test of Intra BC utility:
 * - svt_aom_is_dv_valid
 *
 * @author Cidana-Edmond
 *
 ******************************************************************************/

#include "gtest/gtest.h"
#include "definitions.h"
#include "adaptive_mv_pred.h"
#include "av1me.h"
#include "mcomp.h"
#include "md_process.h"
#include "enc_inter_prediction.h"
#include "util.h"
#include <memory>
#include <vector>

namespace {
using std::make_tuple;

TEST(TileMctsMotionTest, NewMvPredictorsSurviveStandaloneFrameClamping) {
    MacroBlockD xd{};
    xd.tile.mi_col_end = 32;
    xd.tile.mi_row_start = 96;
    xd.tile.mi_row_end = 128;
    xd.mi_col = 28;
    xd.mi_row = 96;
    xd.n8_w = xd.n8_h = 4;
    EXPECT_TRUE(svt_av1_is_mv_predictor_tile_safe(&xd, Mv{{0, -256}}));
    EXPECT_FALSE(svt_av1_is_mv_predictor_tile_safe(&xd, Mv{{0, -376}}));
    EXPECT_TRUE(svt_av1_is_mv_predictor_tile_safe(&xd, Mv{{256, 1152}}));
    EXPECT_FALSE(svt_av1_is_mv_predictor_tile_safe(&xd, Mv{{257, 0}}));
    EXPECT_FALSE(svt_av1_is_mv_predictor_tile_safe(&xd, Mv{{0, 1153}}));
    EXPECT_TRUE(svt_av1_is_mv_predictor_tile_safe(&xd, Mv{{-1152, 0}}));
    EXPECT_FALSE(svt_av1_is_mv_predictor_tile_safe(&xd, Mv{{-1153, 0}}));
}

TEST(TileMctsMotionTest, FractionalFootprintsIncludeChromaAndFilterTaps) {
    TileInfo tile{};
    tile.mi_col_start = tile.mi_row_start = 16;
    tile.mi_col_end = tile.mi_row_end = 48;
    const struct {
        int x, y;
        Mv mv;
        bool safe;
    } cases[] = {{64, 64, {{0, 0}}, true},
                 {64, 64, {{1, 0}}, false},
                 {64, 64, {{-1, 0}}, false},
                 {64, 64, {{8, 0}}, false},
                 {64, 64, {{16, 0}}, true},
                 {64, 64, {{0, 8}}, false},
                 {72, 72, {{1, -1}}, true},
                 {168, 168, {{1, 1}}, true},
                 {176, 176, {{-1, 0}}, false},
                 {176, 176, {{-16, 0}}, true},
                 {176, 176, {{0, -1}}, false},
                 {176, 176, {{0, -16}}, true}};
    for (const auto& test : cases) {
        EXPECT_EQ(
            test.safe,
            svt_av1_is_mv_tile_safe(
                &tile, test.mv, test.x, test.y, 16, 16, 256, 256, SWITCHABLE))
            << test.x << "," << test.y << " MV " << test.mv.x << ","
            << test.mv.y;
    }
}

TEST(TileMctsMotionTest, Sub8BlocksCheckSharedChroma) {
    TileInfo tile{};
    tile.mi_col_start = tile.mi_row_start = 16;
    tile.mi_col_end = tile.mi_row_end = 48;
    EXPECT_TRUE(svt_av1_is_mv_tile_safe(
        &tile, Mv{{0, 0}}, 68, 68, 4, 4, 256, 256, SWITCHABLE));
    EXPECT_FALSE(svt_av1_is_mv_tile_safe(
        &tile, Mv{{-16, 0}}, 68, 68, 4, 4, 256, 256, SWITCHABLE));
    EXPECT_FALSE(svt_av1_is_mv_tile_safe(
        &tile, Mv{{0, -16}}, 68, 68, 4, 4, 256, 256, SWITCHABLE));
}

TEST(TileMctsMotionTest, PartialFrameEdgesDoNotRequireInvisibleSamples) {
    TileInfo tile{};
    tile.mi_col_end = tile.mi_row_end = 64;
    EXPECT_TRUE(svt_av1_is_mv_tile_safe(
        &tile, Mv{{0, 0}}, 240, 240, 16, 16, 250, 250, SWITCHABLE));
    EXPECT_FALSE(svt_av1_is_mv_tile_safe(
        &tile, Mv{{8, 0}}, 240, 240, 16, 16, 250, 250, SWITCHABLE));
    EXPECT_FALSE(svt_av1_is_mv_tile_safe(
        &tile, Mv{{0, 8}}, 240, 240, 16, 16, 250, 250, SWITCHABLE));
    EXPECT_FALSE(svt_av1_is_mv_tile_safe(
        &tile, Mv{{1, 0}}, 248, 240, 16, 16, 250, 250, SWITCHABLE));
}

TEST(TileMctsMotionTest, WarpingChecksEveryProjectedLumaAndChromaBlock) {
    TileInfo tile{};
    tile.mi_col_start = tile.mi_row_start = 16;
    tile.mi_col_end = tile.mi_row_end = 48;
    WarpedMotionParams wm{};
    wm.wmtype = AFFINE;
    wm.wmmat[2] = wm.wmmat[5] = 1 << WARPEDMODEL_PREC_BITS;
    EXPECT_TRUE(
        svt_av1_is_warp_tile_safe(&tile, &wm, 80, 80, 32, 32, 256, 256));
    EXPECT_FALSE(
        svt_av1_is_warp_tile_safe(&tile, &wm, 64, 80, 32, 32, 256, 256));
    EXPECT_FALSE(
        svt_av1_is_warp_tile_safe(&tile, &wm, 80, 64, 32, 32, 256, 256));
    wm.wmmat[0] = 64 << WARPEDMODEL_PREC_BITS;
    EXPECT_FALSE(
        svt_av1_is_warp_tile_safe(&tile, &wm, 80, 80, 64, 32, 256, 256));
}

TEST(TileMctsMotionTest, LocalWarpTranslationClampMustSurviveRelocation) {
    TileInfo tile{};
    tile.mi_col_start = tile.mi_row_start = 16;
    WarpedMotionParams wm{};
    wm.wmtype = AFFINE;
    wm.wmmat[2] = (1 << WARPEDMODEL_PREC_BITS) + 4096;
    wm.wmmat[5] = 1 << WARPEDMODEL_PREC_BITS;
    wm.wmmat[0] = -79 * 4096;
    EXPECT_TRUE(svt_av1_is_local_warp_tile_invariant(
        &tile, &wm, Mv{{0, 0}}, 72, 80, 16, 16));
    tile.mi_col_start = 768;
    wm.wmmat[0] = -WARPEDMODEL_TRANS_CLAMP;
    EXPECT_FALSE(svt_av1_is_local_warp_tile_invariant(
        &tile, &wm, Mv{{0, 0}}, 3080, 80, 16, 16));
}

TEST(TileMctsMotionTest, RefinedCandidatesRecheckChromaFootprints) {
    std::unique_ptr<PictureControlSet> pcs(new PictureControlSet{});
    std::unique_ptr<PictureParentControlSet> parent(
        new PictureParentControlSet{});
    std::unique_ptr<SequenceControlSet> scs(new SequenceControlSet{});
    std::unique_ptr<ModeDecisionContext> ctx(new ModeDecisionContext{});
    pcs->ppcs = parent.get();
    pcs->scs = scs.get();
    scs->static_config.tile_mcts = true;
    scs->static_config.source_width = scs->static_config.source_height = 512;
    parent->frm_hdr.interpolation_filter = SWITCHABLE;
    BlockGeom geometry{};
    geometry.bwidth = geometry.bheight = 16;
    geometry.bsize = BLOCK_16X16;
    MacroBlockD xd{};
    xd.mi_col = 60;
    xd.mi_row = 36;
    xd.n8_w = xd.n8_h = 4;
    xd.tile.mi_col_end = xd.tile.mi_row_end = 64;
    BlkStruct block{};
    block.av1xd = &xd;
    ctx->blk_ptr = &block;
    ctx->blk_geom = &geometry;
    ModeDecisionCandidate candidate{};
    candidate.block_mi.mode = NEWMV;
    candidate.block_mi.ref_frame[0] = LAST_FRAME;
    candidate.block_mi.ref_frame[1] = NONE_FRAME;
    candidate.block_mi.mv[0] = Mv{{-32, -320}};
    EXPECT_TRUE(
        svt_aom_is_inter_candidate_tile_safe(pcs.get(), ctx.get(), &candidate));
    candidate.block_mi.mv[0] = Mv{{-15, -318}};
    EXPECT_FALSE(
        svt_aom_is_inter_candidate_tile_safe(pcs.get(), ctx.get(), &candidate));
}

TEST(TileMctsMotionTest, SubpelSearchKeepsOnlyTileSafeFractionalPositions) {
    TileInfo tile{};
    tile.mi_col_start = tile.mi_row_start = 16;
    tile.mi_col_end = tile.mi_row_end = 48;
    uint8_t pixels[64 * 64] = {};
    svt_buf_2d buffer{pixels + 16 * 64 + 16, 64, 64, 64};
    AomVarianceFnPtr variance{};
    variance.vf = [](const uint8_t*,
                     int,
                     const uint8_t*,
                     int,
                     unsigned int* sse) -> unsigned int {
        *sse = 32;
        return 32;
    };
    variance.svf = [](const uint8_t*,
                      int,
                      int x,
                      int y,
                      const uint8_t*,
                      int,
                      unsigned int* sse) -> unsigned int {
        *sse = (x - 4) * (x - 4) + (y - 4) * (y - 4);
        return *sse;
    };
    SUBPEL_MOTION_SEARCH_PARAMS params{};
    params.forced_stop = HALF_PEL;
    params.iters_per_step = 1;
    params.mv_limits.row_min = params.mv_limits.col_min = -8;
    params.mv_limits.row_max = params.mv_limits.col_max = 8;
    params.mv_cost_params.mv_cost_type = MV_COST_NONE;
    params.var_params.vfp = &variance;
    params.var_params.ms_buffers = {&buffer, &buffer};
    params.var_params.w = params.var_params.h = 16;
    params.var_params.tile = &tile;
    params.var_params.frame_width = params.var_params.frame_height = 256;
    for (const int origin : {16, 20}) {
        params.var_params.mi_row = params.var_params.mi_col = origin;
        Mv best{};
        svt_av1_find_best_sub_pixel_tree_pruned(
            nullptr, nullptr, nullptr, &params, Mv{{0, 0}}, &best, BLOCK_16X16);
        EXPECT_EQ(origin == 16 ? 0 : 4, abs(best.x));
        EXPECT_EQ(origin == 16 ? 0 : 4, abs(best.y));
    }
}

#if CONFIG_ENABLE_OBMC
TEST(TileMctsMotionTest, ObmcChecksNeighborMotionAtTheCurrentBlock) {
    std::unique_ptr<PictureControlSet> pcs(new PictureControlSet{});
    std::unique_ptr<PictureParentControlSet> parent(
        new PictureParentControlSet{});
    std::unique_ptr<SequenceControlSet> scs(new SequenceControlSet{});
    Av1Common cm{};
    pcs->ppcs = parent.get();
    pcs->scs = scs.get();
    parent->av1_cm = &cm;
    cm.mi_cols = cm.mi_rows = 64;
    scs->static_config.source_width = scs->static_config.source_height = 256;
    MbModeInfo neighbor{};
    neighbor.bsize = BLOCK_16X16;
    neighbor.block_mi.mode = NEWMV;
    neighbor.block_mi.ref_frame[0] = LAST_FRAME;
    neighbor.block_mi.ref_frame[1] = NONE_FRAME;
    std::vector<MbModeInfo*> grid(64 * 64, &neighbor);
    MacroBlockD xd{};
    xd.tile.mi_col_start = xd.tile.mi_row_start = 16;
    xd.tile.mi_col_end = xd.tile.mi_row_end = 48;
    xd.mi_stride = 64;
    xd.n4_w = xd.n4_h = 4;
    xd.bsize = BLOCK_16X16;
    for (const bool above : {true, false}) {
        xd.up_available = above;
        xd.left_available = !above;
        xd.mi_col = above ? 20 : 44;
        xd.mi_row = above ? 44 : 20;
        xd.mi = &grid[xd.mi_row * xd.mi_stride + xd.mi_col];
        neighbor.block_mi.mv[0] = Mv{{0, 0}};
        EXPECT_TRUE(svt_aom_is_obmc_tile_safe(pcs.get(), &xd));
        neighbor.block_mi.mv[0] = above ? Mv{{0, 128}} : Mv{{128, 0}};
        EXPECT_FALSE(svt_aom_is_obmc_tile_safe(pcs.get(), &xd));
    }
}
#endif

typedef std::tuple<Mv,            /**< DV */
                   int,           /**< offset of MI row */
                   int,           /**< offset of MI colum */
                   BlockSize, int /**< the result of dv validation function */
                   >
    DvValidationParam;

static const int kSubPelScale = 8;
static const int kTileMaxMibWidth = 8;
static const DvValidationParam dv_validation_params[] = {
    DvValidationParam({{0, 0}}, 0, 0, BLOCK_128X128, 0),
    DvValidationParam({{0, 0}}, 0, 0, BLOCK_64X64, 0),
    DvValidationParam({{0, 0}}, 0, 0, BLOCK_32X32, 0),
    DvValidationParam({{0, 0}}, 0, 0, BLOCK_16X16, 0),
    DvValidationParam({{0, 0}}, 0, 0, BLOCK_8X8, 0),
    DvValidationParam({{0, 0}}, 0, 0, BLOCK_4X4, 0),
    DvValidationParam(
        {{-MAX_SB_SIZE * kSubPelScale, -MAX_SB_SIZE* kSubPelScale}},
        MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE, BLOCK_16X16, 1),
    DvValidationParam({{-MAX_SB_SIZE * kSubPelScale, 0}}, MAX_SB_SIZE / MI_SIZE,
                      MAX_SB_SIZE / MI_SIZE, BLOCK_16X16, 0),
    DvValidationParam({{0, -MAX_SB_SIZE* kSubPelScale}}, MAX_SB_SIZE / MI_SIZE,
                      MAX_SB_SIZE / MI_SIZE, BLOCK_16X16, 1),
    DvValidationParam({{0, MAX_SB_SIZE* kSubPelScale}}, MAX_SB_SIZE / MI_SIZE,
                      MAX_SB_SIZE / MI_SIZE, BLOCK_16X16, 0),
    DvValidationParam({{MAX_SB_SIZE * kSubPelScale, 0}}, MAX_SB_SIZE / MI_SIZE,
                      MAX_SB_SIZE / MI_SIZE, BLOCK_16X16, 0),
    DvValidationParam({{-32 * kSubPelScale, -32 * kSubPelScale}},
                      MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE, BLOCK_32X32,
                      1),
    DvValidationParam({{-32 * kSubPelScale, -32 * kSubPelScale}}, 32 / MI_SIZE,
                      32 / MI_SIZE, BLOCK_32X32, 0),
    DvValidationParam(
        {{-32 * kSubPelScale, -32 * kSubPelScale - kSubPelScale / 2}},
        MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE, BLOCK_32X32, 0),
    DvValidationParam({{-32 * kSubPelScale, -33 * kSubPelScale}},
                      MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE, BLOCK_32X32,
                      1),
    DvValidationParam(
        {{-32 * kSubPelScale - kSubPelScale / 2, -32 * kSubPelScale}},
        MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE, BLOCK_32X32, 0),
    DvValidationParam({{-33 * kSubPelScale, -32 * kSubPelScale}},
                      MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE, BLOCK_32X32,
                      1),
    DvValidationParam(
        {{-MAX_SB_SIZE * kSubPelScale, -MAX_SB_SIZE* kSubPelScale}},
        MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE, BLOCK_LARGEST, 1),
    DvValidationParam(
        {{-MAX_SB_SIZE * kSubPelScale, -(MAX_SB_SIZE + 1) * kSubPelScale}},
        MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE, BLOCK_LARGEST, 0),
    DvValidationParam(
        {{-(MAX_SB_SIZE + 1) * kSubPelScale, -MAX_SB_SIZE* kSubPelScale}},
        MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE, BLOCK_LARGEST, 0),
    DvValidationParam(
        {{-MAX_SB_SIZE * kSubPelScale, -(MAX_SB_SIZE - 1) * kSubPelScale}},
        MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE, BLOCK_LARGEST, 0),
    DvValidationParam(
        {{-(MAX_SB_SIZE - 1) * kSubPelScale, -MAX_SB_SIZE* kSubPelScale}},
        MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE, BLOCK_LARGEST, 1),
    DvValidationParam({{-(MAX_SB_SIZE - 1) * kSubPelScale,
                        -(MAX_SB_SIZE - 1) * kSubPelScale}},
                      MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE,
                      BLOCK_LARGEST, 0),
    DvValidationParam(
        {{MAX_SB_SIZE * kSubPelScale, -MAX_SB_SIZE* kSubPelScale}},
        MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE, BLOCK_LARGEST, 0),
    DvValidationParam({{(kTileMaxMibWidth - 2) * MAX_SB_SIZE * kSubPelScale,
                        -MAX_SB_SIZE* kSubPelScale}},
                      MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE,
                      BLOCK_LARGEST, 0),
    DvValidationParam(
        {{((kTileMaxMibWidth - 2) * MAX_SB_SIZE + 1) * kSubPelScale,
          -MAX_SB_SIZE* kSubPelScale}},
        MAX_SB_SIZE / MI_SIZE, MAX_SB_SIZE / MI_SIZE, BLOCK_LARGEST, 0),
};

class DvValiationTest : public ::testing::TestWithParam<DvValidationParam> {
  public:
    void is_dv_validate() {
        MacroBlockD xd;
        memset(&xd, 0, sizeof(xd));
        xd.tile.mi_row_start = 8 * MAX_MIB_SIZE;
        xd.tile.mi_row_end = 16 * MAX_MIB_SIZE;
        xd.tile.mi_col_start = 24 * MAX_MIB_SIZE;
        xd.tile.mi_col_end =
            xd.tile.mi_col_start + kTileMaxMibWidth * MAX_MIB_SIZE;

        ASSERT_EQ(TEST_GET_PARAM(4),
                  svt_aom_is_dv_valid(TEST_GET_PARAM(0),
                                      &xd,
                                      xd.tile.mi_row_start + TEST_GET_PARAM(1),
                                      xd.tile.mi_col_start + TEST_GET_PARAM(2),
                                      TEST_GET_PARAM(3),
                                      MAX_MIB_SIZE_LOG2));
    }
};

TEST_P(DvValiationTest, IsDvValidate) {
    is_dv_validate();
}

INSTANTIATE_TEST_SUITE_P(AV1, DvValiationTest,
                         ::testing::ValuesIn(dv_validation_params));

}  // namespace
