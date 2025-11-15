/*
* Copyright(c) 2020 Intel Corporation
*
* This source code is subject to the terms of the BSD 2 Clause License and
* the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
* was not distributed with this source code in the LICENSE file, you can
* obtain it at https://www.aomedia.org/license/software-license. If the Alliance for Open
* Media Patent License 1.0 was not distributed with this source code in the
* PATENTS file, you can obtain it at https://www.aomedia.org/license/patent-license.
*/

/*
* This file contains only debug macros that are used during the development
* and are supposed to be cleaned up every tag cycle
* all macros must have the following format:
* - adding a new feature should be prefixed by FTR_
* - tuning a feature should be prefixed by TUNE_
* - enabling a feature should be prefixed by EN_
* - disabling a feature should be prefixed by DIS_
* - bug fixes should be prefixed by FIX_
* - code refactors should be prefixed by RFCTR_
* - code cleanups should be prefixed by CLN_
* - optimizations should be prefixed by OPT_
* - all macros must have a coherent comment explaining what the MACRO is doing
* - #if 0 / #if 1 are not to be used
*/

#ifndef EbDebugMacros_h
#define EbDebugMacros_h

// clang-format off

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#define FIX_TUNE_SSIM               1 // Fix SSIM mode
//FOR DEBUGGING - Do not remove
#define OPT_LD_LATENCY2         1 // Latency optimization for low delay - to keep the Macro for backwards testing until 3.0
#define LOG_ENC_DONE            0 // log encoder job one
#define DEBUG_TPL               0 // Prints to debug TPL
#define DETAILED_FRAME_OUTPUT   0 // Prints detailed frame output from the library for debugging
#define DEBUG_BUFFERS           0 // Print process count and segments info
#define TUNE_CHROMA_SSIM        0 // Allows for Chroma and SSIM BDR-based Tuning
#define TUNE_CQP_CHROMA_SSIM    0 // Tune CQP qp scaling towards improved chroma and SSIM BDR

#define MIN_PIC_PARALLELIZATION 0 // Use the minimum amount of picture parallelization
#define SRM_REPORT              0 // Report SRM status
#define LAD_MG_PRINT            0 // Report LAD
#define RC_NO_R2R               0 // This is a debugging flag for RC and makes encoder to run with no R2R in RC mode
                                  // Note that the speed might impacted significantly
#if !RC_NO_R2R
#define FTR_KF_ON_FLY_SAMPLE      0 // Sample code to signal KF
#define FTR_RES_ON_FLY_SAMPLE     0 // Sample functions to change the resolution on the fly
#define FTR_RATE_ON_FLY_SAMPLE     0 // Sample functions to change bit rate
#endif
// Super-resolution debugging code
#define DEBUG_SCALING           0
#define DEBUG_TF                0
#define DEBUG_SUPERRES_RECODE   0
#define DEBUG_SUPERRES_ENERGY   0
#define DEBUG_RC_CAP_LOG        0 // Prints for RC cap

// Switch frame debugging code
#define DEBUG_SFRAME            0

// Variance boost debugging code
#define DEBUG_VAR_BOOST         0
#define DEBUG_VAR_BOOST_QP      0
#define DEBUG_VAR_BOOST_STATS   0

// Quantization matrices
#define DEBUG_QM_LEVEL          0
#define DEBUG_STARTUP_MG_SIZE   0
#define DEBUG_SEGMENT_QP        0
#define DEBUG_ROI               0

// Hardware config
#define FTR_HW_LIKE_ENCODER   1
#if FTR_HW_LIKE_ENCODER
// High level configuration; Default is a Baseline encoder
// Hardware-oriented ME configuration
#define OPT_HW_HME_ME           1   // Uses a simplified ME pipeline for hardware
                                    // (1) Activates only HME Level 0 and Main ME to capture and refine motion with reduced circuitry
                                    // (2) Employs fixed search windows across all conditions (independent of QP, temporal distance, or MV/distortion feedback)

// Enable promising AV1 tools V0
#define FTR_PROMIZING_AV1_TOOLS_V0 1   // Enable normative tools (Opt-Part, Opt-TXT, intra-prediction for non-square blocks, Warp, TXS, Wiener, and Self-Guided restoration) and non-normative tools (MCTF, TPL)

// Enable promising AV1 tools V1
#define FTR_PROMIZING_AV1_TOOLS_V1 1   // Reduces Angular modes (41 to 25), disables H4/V4 @ 64x64 and 4x4 partitions, limits CDEF to {0,2,4,9} w/o chroma, removes SG filter, and enables IFS(Regular / Smooth), Independent Chroma, and MRP

// Enable promising AV1 tools V2
#define FTR_PROMIZING_AV1_TOOLS_V2 0   // Disable MFMV and MRP


// Parameter-level tuning of normative tools for hardware-like constraints
#define OPT_HW_CDEF           0 // Disable CDEF filter (set to 0 to enable)

#define OPT_HW_PART           1 // Restrict block partitioning to hardware-friendly sizes (set to 0 to enable)
#define OPT_HW_TXT            1 // Disable transform type search (set to 0 to enable)
#define OPT_HW_OBMC           1 // Disable overlapped block motion compensation (set to 0 to enable)
#if !FTR_PROMIZING_AV1_TOOLS_V0
#define OPT_HW_TXS            1 // Disable transform size search (set to 0 to enable)
#define OPT_HW_WARP           1 // Disable warped motion compensation (set to 0 to enable)
#define OPT_HW_INTRA_ONLY_SQ  1 // Disable INTRA for non-square blocks (set to 0 to enable)
#endif
#if !FTR_PROMIZING_AV1_TOOLS_V1
#define OPT_HW_IFS            1 // Disable interpolation filter search (set to 0 to enable)
#endif
#if !FTR_PROMIZING_AV1_TOOLS_V0
#define OPT_HW_WIENER         1 // Disable wiener loop filter (set to 0 to enable)
#endif
#if !FTR_PROMIZING_AV1_TOOLS_V0 || FTR_PROMIZING_AV1_TOOLS_V1
#define OPT_HW_SG             1 // Disable self-guided restoration filter (set to 0 to enable)
#endif
#if !FTR_PROMIZING_AV1_TOOLS_V1
#define OPT_HW_IND            1 // Disable independent chroma prediction modes (set to 0 to enable)
#endif
#define OPT_HW_CFL            1 // Disable chroma-from-luma prediction mode (set to 0 to enable)
#define OPT_HW_INTRA_ANGULAR  1 // Disable angular-intra refinement delta ±2 mode (set to 0 to enable)
#define OPT_HW_INTRA_SMOOTH   1 // Disable smooth-h and smooth-v intra prediction mode (set to 0 to enable)
#define OPT_HW_INTRA_PAETH    1 // Disable paeth intra prediction modes (set to 0 to enable)
#define OPT_HW_INTRA_FILTER   1 // Disable filter-intra prediction mode (set to 0 to enable)
#define OPT_HW_GM             1 // Disable global motion compensation (set to 0 to enable)
#define OPT_HW_INTER_INTRA    1 // Disable inter-intra prediction (set to 0 to enable)
#define OPT_HW_BC_PALETTE     1 // Disable block-copy and palette prediction (set to 0 to enable)
#define OPT_HW_HP             1 // Disable high-precision motion vectors (set to 0 to enable)
#define OPT_HW_EDGE_FILTER    1 // Disable intra-edge filter (set to 0 to enable)
#define OPT_HW_INTER_B4       1 // Disable INTER for 4×4 blocks
#define OPT_HW_COMPOUND       1 // Disable coumpound prediction modes (set to 0 to enable)
#define OPT_HW_NEW_MVP        1 // Disable New/Nearest/Near prediction modes (set to 0 to enable)
#if !FTR_PROMIZING_AV1_TOOLS_V1 || FTR_PROMIZING_AV1_TOOLS_V2
#define OPT_HW_MRP            1 // Disable MRP (set to 0 to enable)
#endif
#if !FTR_PROMIZING_AV1_TOOLS_V0
#define OPT_HW_TF             1 // Disable TF (set to 0 to enable)
#define OPT_HW_TPL            1 // Disable TPL (set to 0 to enable)
#endif
#if FTR_PROMIZING_AV1_TOOLS_V2
#define OPT_HW_MFMV            1 // Disable MFMV (set to 0 to enable)
#endif
// Parameter-level tuning of non-normative tools for hardware-like constraints
#define OPT_HW_RDOQ           1 // Turn OFF RDOQ as it requires sample-based operations that are not hardware-friendly
#define OPT_HW_PME            1 // Turn OFF predictive ME as it may create dependencies that are not hardware-friendly

#if !OPT_HW_HME_ME
// These macros ensure that the MV size does not exceed 192×64
#define OPT_HW_LIMIT_PA_ME    1 // Turn OFF pre-HME and HME; perform a 192×64 full-pel search around (0, 0) to avoid exceeding the reference encoder MV size
#define OPT_HW_BOUND_PA_ME    1 // Crop the ME search area rather than shifting it when it exceeds picture boundaries
#define OPT_HW_LIMIT_PA_MD    1 // Skip refinement for positions beyond the 196×64 range in PME, NSQ-MVs search, and sub-pel search to avoid exceeding the reference encoder MV size
#endif

#define OPT_HW_USE_MIN_RDO    1 // Force a classical 2-stage fast-loop → full-loop scheme, and reduce the number of RDO operations
#if OPT_HW_USE_MIN_RDO // Pick 1 option
#define OPT_HW_1_RDO          0 // Test all candidates at md-stage0, then select the single best candidate for testing at md-stage3
#define OPT_HW_2_RDO          1 // Test candidates per type at md-stage0 (intra, inter), select the best from each type, and test them at md-stage3
#define OPT_HW_3_RDO          0 // Test candidates per class at md-stage0 (intra, new, nearest/near), select the best from each class, and test them at md-stage3
#endif

// Hardware-Friendly AV1 Tools Implementation
#if FTR_PROMIZING_AV1_TOOLS_V0
// Part
#define OPT_HW_AV1_PART_V0    1  // Enable partitions: 64×64, 64×32, 32×64, 64×16, 16×64
#define OPT_HW_AV1_PART_V1    0  // Enable partitions: 32×64, 16×64
// TXT
#define OPT_HW_AV1_TXT_V0     1  // Enable transform search (8 of 16 tx-types): DCT_DCT, V_DCT, H_DCT, ADST_ADST, ADST_DCT, DCT_ADST, FLIPADST_FLIPADST, IDTX
#if FTR_PROMIZING_AV1_TOOLS_V1
#define OPT_HW_AV1_TXT_V1     0  // Enable transform search (4 of 16 tx-types): DCT_DCT, V_DCT, H_DCT, IDTX for INTER only
#endif
// TXS
#if FTR_PROMIZING_AV1_TOOLS_V1
#define OPT_HW_AV1_TXS        0  // Enforce transform size: min 8×8 (unless 4×4 partition is used), max 32×32
#endif
// CDEF
#if FTR_PROMIZING_AV1_TOOLS_V1
#define OPT_HW_NO_CDEF_CHROMA   1  // Disable chroma CDEF filtering
#define OPT_HW_CDEF_LUMA_4S     1  // Use 4 strength sets for luma
#endif

#endif
// Additional tools
#define OPT_HW_MAP_HEVC_QP    0 // Read and force the use of HEVC QP; q-index = 5 × HEVC QP
#define OPT_HW_BOOST_UV       0 // Mimic an HEVC chroma_qp_offset of 12

#endif

#ifdef __cplusplus
}
#endif // __cplusplus

// clang-format on

#endif // EbDebugMacros_h
